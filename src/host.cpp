#include "host.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace service_starter
{
namespace detail
{
namespace
{
// std::stop_source copies share one stop state, so a copy kept here stays valid after the host's own
// instance is gone; the mutex only guards registration against a concurrent request_stop().
std::mutex g_active_mutex;
std::stop_source g_active_stop{std::nostopstate};

using Clock = std::chrono::steady_clock;

// Records when the application last reported progress itself. Automatic checkpoints are only issued
// within progress_timeout of that moment, so a silent hang is eventually visible to the manager.
class ProgressImpl final : public Progress
{
public:
    explicit ProgressImpl(StatusSink& sink) : sink_(sink) { touch(); }
    void report(std::string_view status) override
    {
        touch();
        sink_.progress(status);
    }
    void touch() noexcept { last_explicit_.store(Clock::now().time_since_epoch().count()); }
    bool recently_reported(std::chrono::milliseconds timeout) const noexcept
    {
        const Clock::time_point last{Clock::duration{last_explicit_.load()}};
        return Clock::now() - last <= timeout;
    }

private:
    StatusSink& sink_;
    std::atomic<Clock::rep> last_explicit_{0};
};

// Publishes a pending checkpoint every interval while a long step runs, bounded by the progress timeout.
class Ticker
{
public:
    Ticker(StatusSink& sink, ProgressImpl& progress, std::chrono::milliseconds interval, std::chrono::milliseconds timeout)
        : thread_([&sink, &progress, interval, timeout](std::stop_token stop)
          {
              std::mutex mutex;
              std::condition_variable_any changed;
              std::unique_lock lock{mutex};
              while (!changed.wait_for(lock, stop, interval, [&stop] { return stop.stop_requested(); }))
                  if (progress.recently_reported(timeout))
                      sink.progress({});
          })
    {
        progress.touch(); // A new phase starts with a fresh grace period.
    }

private:
    std::jthread thread_;
};

const char* mode_name(RunMode mode)
{
    return mode == RunMode::service ? "service" : "console";
}
}

ActiveStop::ActiveStop(std::stop_source source) noexcept
{
    std::scoped_lock lock{g_active_mutex};
    g_active_stop = std::move(source);
}

ActiveStop::~ActiveStop()
{
    std::scoped_lock lock{g_active_mutex};
    g_active_stop = std::stop_source{std::nostopstate};
}

bool ActiveStop::request() noexcept
{
    std::stop_source source{std::nostopstate};
    {
        std::scoped_lock lock{g_active_mutex};
        source = g_active_stop;
    }
    if (!source.stop_possible()) return false;
    source.request_stop(); // Outside the lock: stop callbacks may call back into the host.
    return true;
}

Logger make_safe_logger(Logger logger)
{
    if (!logger) return [](LogLevel, std::string_view) {};
    return [logger = std::move(logger)](LogLevel level, std::string_view message) noexcept
    {
        try
        {
            logger(level, message);
        }
        catch (...)
        {
        }
    };
}

std::string describe_exception()
{
    try
    {
        throw;
    }
    catch (const std::system_error& exception)
    {
        return std::string{exception.what()} + " (error " + std::to_string(exception.code().value()) + ")";
    }
    catch (const std::exception& exception)
    {
        return exception.what();
    }
    catch (...)
    {
        return "unknown exception";
    }
}

Logger resolve_logger(const ServiceOptions& options, RunMode mode)
{
    return options.logger ? options.logger : default_logger(mode, options.name);
}

std::string display_name(const ServiceOptions& options)
{
    return options.display_name.empty() ? options.name : options.display_name;
}

std::string quote_for_display(std::string_view argument)
{
    if (!argument.empty() && argument.find_first_of(" \t\"") == argument.npos)
        return std::string{argument};
    std::string out = "\"";
    for (const char c : argument)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '"';
}

ParsedCommandLine parse_command_line(const std::vector<std::string>& args)
{
    ParsedCommandLine parsed;
    bool command_seen = false;
    auto set_command = [&](Command command)
    {
        if (command_seen)
        {
            parsed.command = Command::invalid;
            parsed.error = "Only one host command is allowed.";
            return false;
        }
        command_seen = true;
        parsed.command = command;
        return true;
    };
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& token = args[i];
        if (token == "--")
        {
            parsed.app_args.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
        if (token == "--help" || token == "-h" || token == "/?")
        {
            if (!set_command(Command::help)) return parsed;
        }
        else if (token == "--console")
        {
            if (!set_command(Command::run_console)) return parsed;
        }
        else if (token == "--service")
        {
            if (!set_command(Command::run_service)) return parsed;
        }
        else if (token == "install" || token == "uninstall")
        {
            if (!set_command(token == "install" ? Command::install : Command::uninstall)) return parsed;
            if (i + 1 < args.size() && args[i + 1] != "--" && args[i + 1].rfind("--", 0) != 0)
                parsed.name = args[++i];
        }
        else
        {
            parsed.command = Command::invalid;
            parsed.error = "Unknown argument '" + token + "'. Application arguments must follow \"--\".";
            return parsed;
        }
    }
    return parsed;
}

int run_lifecycle(const ServiceOptions& options, HostContext& context, const ApplicationFactory& factory,
                  StatusSink& sink, std::stop_source stop)
{
    ProgressImpl progress{sink};
    context.progress = &progress;
    context.log = make_safe_logger(context.log); // The application and the host share a logger that cannot throw.
    ActiveStop active{stop};
    const auto interval = options.progress_interval.count() > 0 ? options.progress_interval : std::chrono::milliseconds{1000};
    const auto timeout = options.progress_timeout.count() > 0 ? options.progress_timeout : std::chrono::milliseconds{60000};
    const Logger& log = context.log;

    sink.starting();
    log(LogLevel::info, "Starting " + context.service_name + " (" + mode_name(context.mode) + ")");
    std::unique_ptr<Application> application;
    std::string initialize_failure;
    {
        Ticker ticker{sink, progress, interval, timeout};
        try
        {
            application = factory();
            if (!application)
                throw std::logic_error("application factory returned null");
            application->initialize(context);
        }
        catch (...)
        {
            initialize_failure = describe_exception();
            log(LogLevel::error, "Initialization failed: " + initialize_failure);
            application.reset(); // Release partially acquired resources before the manager learns we stopped.
        }
    }
    if (!initialize_failure.empty())
    {
        sink.stopped(exit_initialize_failed, initialize_failure);
        context.progress = nullptr;
        return exit_initialize_failed;
    }

    sink.running();
    log(LogLevel::info, "Running");
    int exit_code = 0;
    std::string failure;
    try
    {
        exit_code = application->run(stop.get_token());
    }
    catch (...)
    {
        failure = describe_exception();
        exit_code = exit_run_failed;
        log(LogLevel::error, "Run failed: " + failure);
    }

    sink.stopping();
    log(LogLevel::info, "Stopping");
    {
        Ticker ticker{sink, progress, interval, timeout};
        try
        {
            application->shutdown();
        }
        catch (...)
        {
            const auto why = describe_exception();
            log(LogLevel::error, "Shutdown failed: " + why);
            if (exit_code == 0)
            {
                exit_code = exit_shutdown_failed;
                failure = why;
            }
        }
        application.reset(); // Destructors are noexcept; a throwing one terminates regardless of any catch here.
    }
    sink.stopped(exit_code, failure);
    log(exit_code == 0 ? LogLevel::info : LogLevel::warning, "Stopped with exit code " + std::to_string(exit_code));
    context.progress = nullptr;
    return exit_code;
}

int run_main_impl(std::vector<std::string> args, const std::string& program, const ServiceOptions& options,
                  const ApplicationFactory& factory)
{
    if (!factory)
    {
        std::fputs("service_starter: application factory is empty\n", stderr);
        return exit_host_failed;
    }
    const auto parsed = parse_command_line(args);
    try
    {
        switch (parsed.command)
        {
        case Command::help:
            std::fputs(help_text(options, program).c_str(), stdout);
            return 0;
        case Command::invalid:
            std::fprintf(stderr, "%s\n\n%s", parsed.error.c_str(), help_text(options, program).c_str());
            return exit_usage;
        case Command::install:
            return install_service(options, parsed.name.empty() ? options.name : parsed.name, parsed.app_args);
        case Command::uninstall:
            return uninstall_service(options, parsed.name.empty() ? options.name : parsed.name);
        case Command::run_console:
            return run_foreground(options, factory, parsed.app_args, RunMode::console);
        case Command::run_service:
            if (const auto result = run_as_service(options, factory, parsed.app_args, true))
                return *result;
            std::fputs("--service requires a launch by the service manager.\n", stderr);
            return exit_usage;
        case Command::run_auto:
            if (const auto result = run_as_service(options, factory, parsed.app_args, false))
                return *result;
            return run_foreground(options, factory, parsed.app_args, RunMode::console);
        }
    }
    catch (...)
    {
        const auto why = describe_exception();
        make_safe_logger(resolve_logger(options, RunMode::console))(LogLevel::error, "Host failure: " + why);
        return exit_host_failed;
    }
    return exit_host_failed;
}

namespace
{
// systemd unit-file quoting for ExecStart: quote when needed, escape specifiers and variables.
std::string systemd_quote(std::string_view argument)
{
    constexpr std::string_view plain = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./:=+@-";
    const bool needs_quotes = argument.empty() || argument.find_first_not_of(plain) != argument.npos;
    std::string out;
    if (needs_quotes) out += '"';
    for (const char c : argument)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '$': out += "$$"; break;
        case '%': out += "%%"; break;
        case '\n': out += "\\n"; break;
        default: out += c; break;
        }
    }
    if (needs_quotes) out += '"';
    return out;
}
}
}

bool request_stop() noexcept
{
    return detail::ActiveStop::request();
}

int run_main(int argc, char** argv, const ServiceOptions& options, ApplicationFactory factory)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc && argv[i]; ++i)
        args.emplace_back(argv[i]);
    const std::string program = argc > 0 && argv[0] ? std::filesystem::path{argv[0]}.filename().string() : options.name;
    return detail::run_main_impl(std::move(args), program, options, factory);
}

std::string help_text(const ServiceOptions& options, std::string_view program)
{
    std::string text;
    text += "Usage: " + std::string{program} + " [--console | --service | install [NAME] | uninstall [NAME] | --help] [-- application arguments]\n\n";
    text += "  (no command)      Run as a service when started by the service manager, otherwise as a console process.\n";
    text += "  --console         Run in the foreground. Stop with Ctrl+C";
#ifdef _WIN32
    text += " or Ctrl+Break.\n";
#else
    text += ", SIGINT or SIGTERM.\n";
#endif
    text += "  --service         Run as a service; fails when the service manager did not start the process.\n";
#ifdef _WIN32
    text += "  install [NAME]    Register a Windows service (administrator). Default NAME: " + options.name + "\n";
    text += "  uninstall [NAME]  Stop and remove the Windows service.\n";
#else
    text += "  install [NAME]    Write " + (options.unit_directory / (options.name + ".service")).string()
          + " (root). Enabling it stays an operator step. Default NAME: " + options.name + "\n";
    text += "  uninstall [NAME]  Remove the unit file. Stop and disable the unit first.\n";
#endif
    text += "  --help            Show this text.\n\n";
    text += "Arguments after \"--\" are passed to the application and stored with an installed service.\n";
    return text;
}

std::string systemd_unit_text(const ServiceOptions& options, const std::filesystem::path& executable,
                              const std::vector<std::string>& app_args)
{
    std::string exec = detail::systemd_quote(executable.generic_string()) + " --service";
    if (!app_args.empty())
    {
        exec += " --";
        for (const auto& argument : app_args)
            exec += ' ' + detail::systemd_quote(argument);
    }
    std::string text;
    text += "[Unit]\n";
    text += "Description=" + detail::display_name(options) + "\n";
    text += "After=network.target\n\n";
    text += "[Service]\n";
    text += "Type=notify\n";
    text += "ExecStart=" + exec + "\n";
    if (options.change_directory_to_executable)
        text += "WorkingDirectory=" + detail::systemd_quote(executable.parent_path().generic_string()) + "\n";
    if (!options.unit_user.empty())
    {
        text += "User=" + options.unit_user + "\n";
        text += "Group=" + (options.unit_group.empty() ? options.unit_user : options.unit_group) + "\n";
    }
    text += "Restart=on-failure\n";
    text += "RestartSec=" + std::to_string(options.restart_delay.count()) + "\n";
    text += "TimeoutStopSec=" + std::to_string(options.stop_timeout.count()) + "\n";
    if (options.watchdog.count() > 0)
        text += "WatchdogSec=" + std::to_string(options.watchdog.count()) + "\n";
    if (options.unit_hardening)
    {
        text += "NoNewPrivileges=true\n";
        text += "PrivateTmp=true\n";
        text += "ProtectSystem=strict\n";
        text += "ProtectHome=true\n";
    }
    text += "\n[Install]\n";
    text += "WantedBy=multi-user.target\n";
    return text;
}
}
