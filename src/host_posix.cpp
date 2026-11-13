#include "host.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace service_starter
{
namespace detail
{
namespace
{
[[noreturn]] void fail(const char* operation, int error = errno)
{
    throw std::system_error(error, std::generic_category(), operation);
}

void write_line(FILE* stream, std::string_view text)
{
    std::string line{text};
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stream);
    std::fflush(stream);
}

// sd-daemon log level prefixes: journald strips them and records the priority.
const char* journal_prefix(LogLevel level)
{
    switch (level)
    {
    case LogLevel::debug: return "<7>";
    case LogLevel::info: return "<6>";
    case LogLevel::warning: return "<4>";
    case LogLevel::error: return "<3>";
    }
    return "<6>";
}

const char* level_label(LogLevel level)
{
    switch (level)
    {
    case LogLevel::debug: return "[debug] ";
    case LogLevel::info: return "[info] ";
    case LogLevel::warning: return "[warning] ";
    case LogLevel::error: return "[error] ";
    }
    return "";
}

sigset_t stop_signal_set()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    return set;
}

const char* signal_name(int signal)
{
    switch (signal)
    {
    case SIGINT: return "SIGINT";
    case SIGTERM: return "SIGTERM";
    case SIGHUP: return "SIGHUP";
    }
    return "signal";
}

// Blocks the stop signals in the constructing thread (new threads inherit the mask) and consumes them
// with sigtimedwait on a dedicated thread, so no async-signal-safety constraints apply to the stop path.
class SignalWaiter
{
public:
    SignalWaiter(std::stop_source& stop, const Logger& log)
    {
        set_ = stop_signal_set();
        if (const int error = pthread_sigmask(SIG_BLOCK, &set_, &previous_mask_))
            fail("pthread_sigmask", error);
        // A signal whose disposition is SIG_IGN is discarded even while blocked, so sigtimedwait would never
        // see it. Background jobs of non-interactive shells and nohup start with SIGINT/SIGHUP ignored.
        for (std::size_t i = 0; i < std::size(stop_signals_); ++i)
        {
            struct sigaction current{};
            if (sigaction(stop_signals_[i], nullptr, &current) == 0 && current.sa_handler == SIG_IGN)
            {
                struct sigaction standard{};
                standard.sa_handler = SIG_DFL;
                sigemptyset(&standard.sa_mask);
                if (sigaction(stop_signals_[i], &standard, nullptr) == 0) restore_default_[i] = true;
            }
        }
        previous_pipe_ = std::signal(SIGPIPE, SIG_IGN);
        thread_ = std::jthread([this, &stop, &log](std::stop_token token)
        {
            const timespec poll{0, 100'000'000};
            while (!token.stop_requested())
            {
                siginfo_t info{};
                const int signal = sigtimedwait(&set_, &info, &poll);
                if (signal > 0)
                {
                    log(LogLevel::info, std::string{"Received "} + signal_name(signal) + (stop.stop_requested() ? "; shutdown already in progress" : ""));
                    stop.request_stop();
                }
                else if (errno != EAGAIN && errno != EINTR)
                {
                    log(LogLevel::warning, "sigtimedwait failed; signals are no longer handled (error " + std::to_string(errno) + ")");
                    return;
                }
            }
        });
    }

    ~SignalWaiter()
    {
        thread_.request_stop();
        thread_.join();
        if (previous_pipe_ != SIG_ERR) std::signal(SIGPIPE, previous_pipe_);
        for (std::size_t i = 0; i < std::size(stop_signals_); ++i)
            if (restore_default_[i]) std::signal(stop_signals_[i], SIG_IGN);
        pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
    }

    SignalWaiter(const SignalWaiter&) = delete;
    SignalWaiter& operator=(const SignalWaiter&) = delete;

private:
    static constexpr int stop_signals_[] = {SIGINT, SIGTERM, SIGHUP};
    bool restore_default_[std::size(stop_signals_)] = {};
    sigset_t set_{};
    sigset_t previous_mask_{};
    void (*previous_pipe_)(int) = SIG_ERR;
    std::jthread thread_;
};

// sd_notify without libsystemd: datagrams to $NOTIFY_SOCKET. Silent no-op when not under systemd.
class SystemdNotifier
{
public:
    explicit SystemdNotifier(const Logger& log)
    {
        const char* path = std::getenv("NOTIFY_SOCKET");
        if (!path || !*path) return;
        const auto length = std::strlen(path);
        if ((path[0] != '/' && path[0] != '@') || length >= sizeof(sockaddr_un::sun_path))
        {
            log(LogLevel::warning, "NOTIFY_SOCKET is not a usable address; readiness will not be reported");
            return;
        }
        // Non-blocking: a full receive queue must never stall startup or shutdown; a dropped status line is harmless.
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0)
        {
            log(LogLevel::warning, "Cannot open the notification socket (error " + std::to_string(errno) + ")");
            return;
        }
        address_.sun_family = AF_UNIX;
        std::memcpy(address_.sun_path, path, length);
        if (path[0] == '@') address_.sun_path[0] = '\0';
        address_length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + length);

        if (const char* usec = std::getenv("WATCHDOG_USEC"))
        {
            const char* pid = std::getenv("WATCHDOG_PID");
            if (!pid || std::strtol(pid, nullptr, 10) == static_cast<long>(::getpid()))
            {
                const auto micro = std::strtoull(usec, nullptr, 10);
                if (micro > 0) watchdog_interval_ = std::chrono::microseconds{micro / 2};
            }
        }
    }

    ~SystemdNotifier()
    {
        if (fd_ >= 0) ::close(fd_);
    }

    SystemdNotifier(const SystemdNotifier&) = delete;
    SystemdNotifier& operator=(const SystemdNotifier&) = delete;

    bool enabled() const noexcept { return fd_ >= 0; }
    std::chrono::microseconds watchdog_interval() const noexcept { return watchdog_interval_; }

    void notify(std::string_view message) const noexcept
    {
        if (fd_ < 0) return;
        ::sendto(fd_, message.data(), message.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&address_), address_length_);
    }

private:
    int fd_ = -1;
    sockaddr_un address_{};
    socklen_t address_length_ = 0;
    std::chrono::microseconds watchdog_interval_{0};
};

class Watchdog
{
public:
    explicit Watchdog(const SystemdNotifier& notifier)
    {
        if (!notifier.enabled() || notifier.watchdog_interval().count() <= 0) return;
        thread_ = std::jthread([&notifier](std::stop_token token)
        {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::unique_lock lock{mutex};
            while (!changed.wait_for(lock, token, notifier.watchdog_interval(), [&token] { return token.stop_requested(); }))
                notifier.notify("WATCHDOG=1");
        });
    }

private:
    std::jthread thread_;
};

class SystemdSink final : public StatusSink
{
public:
    SystemdSink(const SystemdNotifier& notifier, const ServiceOptions& options) : notifier_(notifier)
    {
        const auto interval = options.progress_interval.count() > 0 ? options.progress_interval : std::chrono::milliseconds{1000};
        const auto extension = std::max<std::chrono::milliseconds>(interval * 3, std::chrono::milliseconds{3000});
        extend_ = "EXTEND_TIMEOUT_USEC=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(extension).count());
    }
    void starting() override
    {
        pending_.store(true);
        notifier_.notify("STATUS=Starting");
    }
    // Mirrors the SCM checkpoint: each progress tick pushes the start/stop timeout out by a bounded amount.
    void progress(std::string_view status) override
    {
        std::string message;
        if (pending_.load()) message = extend_;
        if (!status.empty()) message += (message.empty() ? "" : "\n") + std::string{"STATUS="} + std::string{status};
        if (!message.empty()) notifier_.notify(message);
    }
    void running() override
    {
        pending_.store(false);
        notifier_.notify("READY=1\nSTATUS=Running");
    }
    void stopping() override
    {
        pending_.store(true);
        notifier_.notify("STOPPING=1\nSTATUS=Stopping");
    }
    void stopped(int exit_code, std::string_view failure) override
    {
        pending_.store(false);
        std::string status = "STATUS=Stopped (exit code " + std::to_string(exit_code) + ")";
        if (!failure.empty()) status += ": " + std::string{failure};
        notifier_.notify(status);
    }

private:
    const SystemdNotifier& notifier_;
    std::string extend_;
    std::atomic<bool> pending_{false};
};

bool under_service_manager()
{
    const char* notify = std::getenv("NOTIFY_SOCKET");
    const char* invocation = std::getenv("INVOCATION_ID");
    return (notify && *notify) || (invocation && *invocation);
}

std::string validated_unit_name(const std::string& name)
{
    constexpr std::string_view allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:_.-@";
    if (name.empty() || name.size() > 255 || name.find_first_not_of(allowed) != name.npos || name.front() == '-')
        throw std::invalid_argument("Unit name must use [A-Za-z0-9:_.@-] and must not start with '-'");
    constexpr std::string_view suffix = ".service";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        return name;
    return name + std::string{suffix};
}
}

Logger default_logger(RunMode mode, std::string_view)
{
    return [mode](LogLevel level, std::string_view message)
    {
        FILE* stream = level >= LogLevel::warning ? stderr : stdout;
        write_line(stream, std::string{mode == RunMode::service ? journal_prefix(level) : level_label(level)} + std::string{message});
    };
}

std::filesystem::path executable_path()
{
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return path;
    fail("read_symlink(/proc/self/exe)", error.value());
}

std::optional<int> run_as_service(const ServiceOptions& options, const ApplicationFactory& factory,
                                  const std::vector<std::string>& app_args, bool require_manager)
{
    if (!require_manager && !under_service_manager())
        return std::nullopt;
    return run_foreground(options, factory, app_args, RunMode::service);
}

int install_service(const ServiceOptions& options, const std::string& name, const std::vector<std::string>& app_args)
{
    const auto unit = validated_unit_name(name);
    const auto path = options.unit_directory / unit;
    if (std::filesystem::exists(path))
    {
        write_line(stderr, path.string() + " already exists. Run uninstall first.");
        return exit_usage;
    }
    const auto text = systemd_unit_text(options, executable_path(), app_args);
    {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) fail("open unit file for writing (root required?)");
        out << text;
        if (!out) fail("write unit file");
    }
    ::chmod(path.c_str(), 0644);
    write_line(stdout, "Wrote " + path.string());
    write_line(stdout, "Next: sudo systemctl daemon-reload && sudo systemctl enable --now " + unit);
    return 0;
}

int uninstall_service(const ServiceOptions& options, const std::string& name)
{
    const auto unit = validated_unit_name(name);
    const auto path = options.unit_directory / unit;
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        write_line(stderr, path.string() + " does not exist.");
        return exit_usage;
    }
    if (!std::filesystem::remove(path, error)) fail("remove unit file", error.value());
    write_line(stdout, "Removed " + path.string());
    write_line(stdout, "Next: sudo systemctl daemon-reload (stop and disable the unit first if it is active)");
    return 0;
}
}

int run_foreground(const ServiceOptions& options, ApplicationFactory factory, std::vector<std::string> app_args, RunMode mode)
{
    if (!factory)
        throw std::invalid_argument("application factory is empty");
    HostContext context;
    context.mode = mode;
    context.service_name = options.name;
    context.executable = detail::executable_path();
    context.args = std::move(app_args);
    context.log = detail::resolve_logger(options, mode);
    if (options.change_directory_to_executable)
        std::filesystem::current_path(context.executable.parent_path());

    std::stop_source stop;
    detail::SignalWaiter signals{stop, context.log}; // Before the application creates threads.
    if (mode == RunMode::service)
    {
        detail::SystemdNotifier notifier{context.log};
        detail::Watchdog watchdog{notifier};
        detail::SystemdSink sink{notifier, options};
        return detail::run_lifecycle(options, context, factory, sink, stop);
    }
    detail::NullSink sink;
    return detail::run_lifecycle(options, context, factory, sink, stop);
}
}
