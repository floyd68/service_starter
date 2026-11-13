#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "host.hpp"
#include "service_starter/windows.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace service_starter
{
namespace detail
{
namespace
{
[[noreturn]] void fail(const char* operation, DWORD error = GetLastError())
{
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

std::wstring to_wide(std::string_view utf8)
{
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!size) fail("MultiByteToWideChar");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string to_utf8(std::wstring_view wide)
{
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (!size) fail("WideCharToMultiByte");
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

void write_utf8(FILE* stream, std::string_view text)
{
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

const char* level_label(LogLevel level)
{
    switch (level)
    {
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    }
    return "log";
}

// Service-mode logger: debugger output for everything, Application event log for warnings and errors.
Logger event_log_logger(std::wstring source)
{
    return [source = std::move(source)](LogLevel level, std::string_view message)
    {
        const auto wide = to_wide(message) + L"\n";
        OutputDebugStringW(wide.c_str());
        if (level < LogLevel::warning) return;
        if (HANDLE handle = RegisterEventSourceW(nullptr, source.c_str()))
        {
            const wchar_t* strings[] = {wide.c_str()};
            ReportEventW(handle, level == LogLevel::error ? EVENTLOG_ERROR_TYPE : EVENTLOG_WARNING_TYPE, 0, 0, nullptr, 1, 0, strings, nullptr);
            DeregisterEventSource(handle);
        }
    };
}

// Console mode: the handler runs on a system thread. For close/logoff/shutdown the process is killed once
// the handler returns, so wait (bounded) for the lifecycle to finish.
std::atomic<bool> g_console_lifecycle_done{false};

BOOL WINAPI console_control_handler(DWORD type)
{
    switch (type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        ActiveStop::request();
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    {
        ActiveStop::request();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{4500};
        while (!g_console_lifecycle_done.load() && std::chrono::steady_clock::now() < deadline)
            Sleep(50);
        return TRUE;
    }
    default:
        return FALSE;
    }
}

using ServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, decltype(&CloseServiceHandle)>;

SERVICE_STATUS_PROCESS query_status(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), static_cast<DWORD>(sizeof(status)), &bytes))
        fail("QueryServiceStatusEx");
    return status;
}

std::wstring validated_service_name(const std::string& name)
{
    const auto wide = to_wide(name);
    if (wide.empty() || wide.size() > 256 || wide.find_first_of(L"/\\") != wide.npos)
        throw std::invalid_argument("Service name must contain 1..256 characters without slashes");
    return wide;
}

DWORD to_milliseconds(std::chrono::milliseconds duration)
{
    return static_cast<DWORD>(std::clamp<long long>(duration.count(), 0, 0x7fffffff));
}

// Hosts one SCM service. Owns status reporting; the lifecycle drives it through Sink.
class ScmHost
{
public:
    ScmHost(const ServiceOptions& options, const ApplicationFactory& factory, std::vector<std::string> app_args, windows::ScmApi api)
        : options_(options), factory_(factory), app_args_(std::move(app_args)), api_(api)
    {
    }

    std::optional<int> run()
    {
        ScmHost* expected = nullptr;
        if (!active_.compare_exchange_strong(expected, this))
            throw std::logic_error("Only one service host can run per process");
        auto table_name = to_wide(options_.name); // Ignored for SERVICE_WIN32_OWN_PROCESS but must not be null.
        SERVICE_TABLE_ENTRYW table[] = {{table_name.data(), &service_main}, {nullptr, nullptr}};
        const auto dispatched = api_.dispatch(table);
        const auto error = dispatched ? NO_ERROR : api_.last_error();
        active_.store(nullptr);
        if (!dispatched)
        {
            if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
                return std::nullopt;
            fail("StartServiceCtrlDispatcherW", error);
        }
        const auto code = exit_code_.load();
        return code == 0 && scm_error_.load() != NO_ERROR ? exit_host_failed : code;
    }

private:
    // Every transition is reported under mutex_; the stop request that follows a failed report is issued
    // after the lock is released, because std::stop_callback handlers run synchronously and may call back
    // into Progress::report() (which takes the same lock).
    class Sink final : public StatusSink
    {
    public:
        explicit Sink(ScmHost& host) : host_(host) {}
        void starting() override
        {
            {
                std::scoped_lock lock{host_.mutex_};
                host_.report(SERVICE_START_PENDING);
            }
            stop_if_unreported();
        }
        void progress(std::string_view) override
        {
            {
                std::scoped_lock lock{host_.mutex_};
                const auto state = host_.status_.dwCurrentState;
                if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
                    host_.report(state);
            }
            stop_if_unreported();
        }
        void running() override
        {
            {
                std::scoped_lock lock{host_.mutex_};
                host_.report(SERVICE_RUNNING);
            }
            // If SCM never learned that we run, it cannot stop us either: unwind instead of running unmanaged.
            stop_if_unreported();
        }
        void stopping() override
        {
            std::scoped_lock lock{host_.mutex_};
            if (host_.status_.dwCurrentState != SERVICE_STOP_PENDING) // The control handler may have reported it already.
                host_.report(SERVICE_STOP_PENDING);
        }
        void stopped(int exit_code, std::string_view) override
        {
            std::scoped_lock lock{host_.mutex_};
            host_.exit_code_.store(exit_code);
            if (const auto scm_error = host_.scm_error_.load(); scm_error != NO_ERROR)
                host_.report(SERVICE_STOPPED, scm_error, 0);
            else if (exit_code != 0)
                host_.report(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(exit_code));
            else
                host_.report(SERVICE_STOPPED);
        }

    private:
        void stop_if_unreported()
        {
            if (host_.scm_error_.load() != NO_ERROR)
                host_.stop_.request_stop();
        }
        ScmHost& host_;
    };

    static void WINAPI service_main(DWORD argc, wchar_t** argv) noexcept
    {
        if (auto* host = active_.load())
            host->serve(argc, argv);
    }

    static DWORD WINAPI control_handler(DWORD control, DWORD, void*, void* context) noexcept
    {
        return static_cast<ScmHost*>(context)->handle_control(control);
    }

    // Caller holds mutex_. Failures are recorded, not thrown: the lifecycle must still reach STOPPED.
    void report(DWORD state, DWORD win32_exit = NO_ERROR, DWORD specific_exit = 0)
    {
        SERVICE_STATUS next{};
        next.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        next.dwCurrentState = state;
        next.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN : 0;
        next.dwWin32ExitCode = win32_exit;
        next.dwServiceSpecificExitCode = specific_exit;
        if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        {
            next.dwCheckPoint = status_.dwCurrentState == state ? status_.dwCheckPoint + 1 : 1;
            const auto interval = options_.progress_interval.count() > 0 ? options_.progress_interval : std::chrono::milliseconds{1000};
            auto hint = std::max<std::chrono::milliseconds>(interval * 3, std::chrono::milliseconds{3000});
            if (state == SERVICE_STOP_PENDING) // run() may drain for a while before the shutdown ticker starts.
                hint = std::max<std::chrono::milliseconds>(hint, options_.stop_timeout);
            next.dwWaitHint = to_milliseconds(hint);
        }
        if (!api_.set_status(handle_, &next))
        {
            const auto error = api_.last_error();
            scm_error_.store(error);
            if (log_) log_(LogLevel::error, "SetServiceStatus failed (error " + std::to_string(error) + ")");
            return;
        }
        status_ = next;
    }

    DWORD handle_control(DWORD control) noexcept
    {
        if (control == SERVICE_CONTROL_INTERROGATE)
            return NO_ERROR;
        if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN && control != SERVICE_CONTROL_PRESHUTDOWN)
            return ERROR_CALL_NOT_IMPLEMENTED;
        try
        {
            {
                std::scoped_lock lock{mutex_};
                const auto state = status_.dwCurrentState;
                if (state == SERVICE_STOP_PENDING || state == SERVICE_STOPPED)
                    return NO_ERROR;
                if (state != SERVICE_RUNNING)
                    return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
                report(SERVICE_STOP_PENDING);
            }
            stop_.request_stop(); // Lock released: stop callbacks may call Progress::report().
            return NO_ERROR;
        }
        catch (...)
        {
            return ERROR_EXCEPTION_IN_SERVICE;
        }
    }

    void serve(DWORD argc, wchar_t** argv) noexcept
    {
        const std::wstring name = argc && argv && argv[0] ? argv[0] : to_wide(options_.name);
        handle_ = api_.register_handler(name.c_str(), &control_handler, this);
        if (!handle_)
        {
            scm_error_.store(api_.last_error());
            exit_code_.store(exit_host_failed);
            OutputDebugStringW(L"service_starter: RegisterServiceCtrlHandlerExW failed\n");
            return;
        }
        Sink sink{*this};
        try
        {
            log_ = options_.logger ? options_.logger : event_log_logger(name);
            HostContext context;
            context.mode = RunMode::service;
            context.service_name = to_utf8(name);
            context.executable = executable_path();
            context.args = app_args_;
            context.log = log_;
            if (options_.change_directory_to_executable)
                std::filesystem::current_path(context.executable.parent_path());
            exit_code_.store(run_lifecycle(options_, context, factory_, sink, stop_));
        }
        catch (...)
        {
            const auto why = describe_exception();
            if (log_) log_(LogLevel::error, "Service host failed: " + why);
            sink.stopped(exit_host_failed, why);
        }
    }

    static std::atomic<ScmHost*> active_;
    const ServiceOptions& options_;
    const ApplicationFactory& factory_;
    std::vector<std::string> app_args_;
    windows::ScmApi api_;
    std::mutex mutex_;
    SERVICE_STATUS_HANDLE handle_ = nullptr;
    SERVICE_STATUS status_{};
    std::stop_source stop_;
    std::atomic<DWORD> scm_error_{NO_ERROR};
    std::atomic<int> exit_code_{0};
    Logger log_;
};

std::atomic<ScmHost*> ScmHost::active_{nullptr};

// CommandLineToArgvW-compatible quoting (the "everyone quotes arguments the wrong way" algorithm).
std::wstring quote_argument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == argument.npos)
        return std::wstring{argument};
    std::wstring out = L"\"";
    for (auto it = argument.begin();; ++it)
    {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\')
        {
            ++it;
            ++backslashes;
        }
        if (it == argument.end())
        {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}
}

Logger default_logger(RunMode mode, std::string_view service_name)
{
    if (mode == RunMode::service)
        return event_log_logger(to_wide(service_name));
    return [](LogLevel level, std::string_view message)
    {
        write_utf8(level >= LogLevel::warning ? stderr : stdout, std::string{"["} + level_label(level) + "] " + std::string{message});
    };
}

std::filesystem::path executable_path()
{
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) fail("GetModuleFileNameW");
        if (size < buffer.size()) return std::wstring{buffer.data(), size};
        if (buffer.size() >= 32768) throw std::length_error("Executable path is too long");
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::optional<int> run_as_service(const ServiceOptions& options, const ApplicationFactory& factory,
                                  const std::vector<std::string>& app_args, bool)
{
    return windows::try_run_as_service(options, factory, app_args);
}

int install_service(const ServiceOptions& options, const std::string& name, const std::vector<std::string>& app_args)
{
    SetConsoleOutputCP(CP_UTF8);
    const auto wide_name = validated_service_name(name);
    const auto wide_display = to_wide(options.display_name.empty() ? name : options.display_name);
    const auto command = windows::service_command_line(executable_path(), app_args);
    std::wstring dependencies;
    for (const auto& dependency : options.dependencies)
        dependencies += to_wide(dependency) + L'\0';
    const auto account = options.account.empty() ? std::wstring{} : to_wide(options.account);

    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE), &CloseServiceHandle};
    if (!manager) fail("OpenSCManagerW (install requires administrator rights)");
    // SERVICE_START is required to register SC_ACTION_RESTART recovery actions; DELETE allows rollback.
    ServiceHandle service{CreateServiceW(manager.get(), wide_name.c_str(), wide_display.c_str(),
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | DELETE,
        SERVICE_WIN32_OWN_PROCESS, options.auto_start ? SERVICE_AUTO_START : SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(),
        nullptr, nullptr, dependencies.empty() ? nullptr : (dependencies + L'\0').c_str(), account.empty() ? nullptr : account.c_str(), nullptr),
        &CloseServiceHandle};
    if (!service) fail("CreateServiceW");

    if (!options.description.empty())
    {
        auto description = to_wide(options.description);
        SERVICE_DESCRIPTIONW info{description.data()};
        if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &info))
            write_utf8(stderr, "Warning: service description was not set (error " + std::to_string(GetLastError()) + ")");
    }
    const auto delay = to_milliseconds(options.restart_delay);
    SC_ACTION actions[] = {{SC_ACTION_RESTART, delay}, {SC_ACTION_RESTART, delay}, {SC_ACTION_RESTART, delay}};
    SERVICE_FAILURE_ACTIONSW recovery{};
    recovery.dwResetPeriod = 86400;
    recovery.cActions = static_cast<DWORD>(std::size(actions));
    recovery.lpsaActions = actions;
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE}; // Also recover from a non-zero exit, not only from a crash.
    if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &recovery)
        || !ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag))
    {
        const auto error = GetLastError();
        if (!DeleteService(service.get()))
            write_utf8(stderr, "Warning: rollback failed; service " + name + " is installed without recovery actions (error " + std::to_string(GetLastError()) + ")");
        fail("ChangeServiceConfig2W (recovery actions); installation rolled back", error);
    }

    write_utf8(stdout, "Installed service " + name + ": " + to_utf8(command));
    write_utf8(stdout, std::string{"Start type: "} + (options.auto_start ? "automatic" : "manual") + ". Start with: sc start " + quote_for_display(name));
    return 0;
}

int uninstall_service(const ServiceOptions& options, const std::string& name)
{
    SetConsoleOutputCP(CP_UTF8);
    const auto wide_name = validated_service_name(name);
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT), &CloseServiceHandle};
    if (!manager) fail("OpenSCManagerW (uninstall)");
    ServiceHandle service{OpenServiceW(manager.get(), wide_name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE), &CloseServiceHandle};
    if (!service) fail("OpenServiceW (uninstall)");

    auto status = query_status(service.get());
    if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING)
    {
        SERVICE_STATUS ignored{};
        if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored))
        {
            const auto error = GetLastError();
            if (error != ERROR_SERVICE_NOT_ACTIVE) fail("ControlService (STOP)", error);
        }
        write_utf8(stdout, "Stopping " + name + " ...");
    }
    const auto deadline = std::chrono::steady_clock::now() + options.stop_timeout + std::chrono::seconds{5};
    while (query_status(service.get()).dwCurrentState != SERVICE_STOPPED)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            fail("Timed out waiting for the service to stop; it was not deleted", ERROR_TIMEOUT);
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    if (!DeleteService(service.get())) fail("DeleteService");
    write_utf8(stdout, "Removed service " + name);
    return 0;
}
}

namespace windows
{
std::optional<int> try_run_as_service(const ServiceOptions& options, const ApplicationFactory& factory,
                                      const std::vector<std::string>& app_args, ScmApi api)
{
    detail::ScmHost host{options, factory, app_args, api};
    return host.run();
}

std::wstring service_command_line(const std::filesystem::path& executable, const std::vector<std::string>& app_args)
{
    const auto path = executable.wstring();
    if (path.empty() || path.find(L'"') != path.npos)
        throw std::invalid_argument("Invalid executable path");
    std::wstring command = L"\"" + path + L"\" --service";
    if (!app_args.empty())
    {
        command += L" --";
        for (const auto& argument : app_args)
            command += L' ' + detail::quote_argument(detail::to_wide(argument));
    }
    return command;
}
}

int run_foreground(const ServiceOptions& options, ApplicationFactory factory, std::vector<std::string> app_args, RunMode mode)
{
    if (mode != RunMode::console)
        throw std::invalid_argument("On Windows, service mode is entered through SCM (run_main); run_foreground supports console mode only");
    if (!factory)
        throw std::invalid_argument("application factory is empty");
    SetConsoleOutputCP(CP_UTF8);
    HostContext context;
    context.mode = RunMode::console;
    context.service_name = options.name;
    context.executable = detail::executable_path();
    context.args = std::move(app_args);
    context.log = detail::resolve_logger(options, RunMode::console);
    if (options.change_directory_to_executable)
        std::filesystem::current_path(context.executable.parent_path());

    std::stop_source stop;
    detail::NullSink sink;
    detail::g_console_lifecycle_done.store(false);
    SetConsoleCtrlHandler(&detail::console_control_handler, TRUE);
    const auto exit_code = detail::run_lifecycle(options, context, factory, sink, stop);
    detail::g_console_lifecycle_done.store(true);
    SetConsoleCtrlHandler(&detail::console_control_handler, FALSE);
    return exit_code;
}

int run_main(int argc, wchar_t** argv, const ServiceOptions& options, ApplicationFactory factory)
{
    std::vector<std::string> args;
    std::string program = options.name;
    try
    {
        for (int i = 1; i < argc && argv[i]; ++i)
            args.push_back(detail::to_utf8(argv[i]));
        if (argc > 0 && argv[0])
            program = detail::to_utf8(std::filesystem::path{argv[0]}.filename().wstring());
    }
    catch (...)
    {
        std::fputs("service_starter: command line is not valid UTF-16\n", stderr);
        return exit_usage;
    }
    return detail::run_main_impl(std::move(args), program, options, factory);
}
}
