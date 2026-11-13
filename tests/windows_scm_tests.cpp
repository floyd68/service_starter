// Drives the SCM lifecycle through a fake service control manager (no privileges required).
#include "service_starter/windows.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct FakeScm
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<SERVICE_STATUS> statuses;
    LPHANDLER_FUNCTION_EX handler = nullptr;
    void* context = nullptr;
    DWORD dispatch_error = NO_ERROR;
    bool registration_fails = false;
    DWORD fail_state = 0;
    DWORD control = SERVICE_CONTROL_STOP;
    bool ready = false;
    bool done = false;
};
FakeScm* scm = nullptr;

DWORD WINAPI fake_last_error()
{
    return scm->dispatch_error ? scm->dispatch_error : ERROR_ACCESS_DENIED;
}

SERVICE_STATUS_HANDLE WINAPI fake_register(LPCWSTR name, LPHANDLER_FUNCTION_EX handler, LPVOID context)
{
    check(std::wstring_view{name} == L"ServiceStarter 테스트", "must register the SCM-provided service name");
    if (scm->registration_fails) return nullptr;
    scm->handler = handler;
    scm->context = context;
    check(handler(SERVICE_CONTROL_INTERROGATE, 0, nullptr, context) == NO_ERROR, "early interrogate");
    check(handler(SERVICE_CONTROL_STOP, 0, nullptr, context) == ERROR_SERVICE_CANNOT_ACCEPT_CTRL, "stop before RUNNING is refused");
    return reinterpret_cast<SERVICE_STATUS_HANDLE>(scm);
}

BOOL WINAPI fake_set_status(SERVICE_STATUS_HANDLE handle, LPSERVICE_STATUS status)
{
    check(handle == reinterpret_cast<SERVICE_STATUS_HANDLE>(scm), "status handle");
    std::scoped_lock lock{scm->mutex};
    if (scm->fail_state == status->dwCurrentState) return FALSE;
    check(scm->statuses.empty() || scm->statuses.back().dwCurrentState != SERVICE_STOPPED, "no report after STOPPED");
    if (status->dwCurrentState == SERVICE_START_PENDING || status->dwCurrentState == SERVICE_STOP_PENDING)
        check(status->dwControlsAccepted == 0 && status->dwCheckPoint > 0 && status->dwWaitHint > 0, "pending status fields");
    else
        check(status->dwCheckPoint == 0 && status->dwWaitHint == 0, "non-pending checkpoint/hint are zero");
    if (!scm->statuses.empty() && scm->statuses.back().dwCurrentState == status->dwCurrentState && status->dwCurrentState == SERVICE_START_PENDING)
        check(status->dwCheckPoint == scm->statuses.back().dwCheckPoint + 1, "checkpoint increments");
    if (status->dwCurrentState == SERVICE_RUNNING)
    {
        check(status->dwControlsAccepted == (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN), "accepts stop/shutdown/preshutdown");
        scm->ready = true;
    }
    if (status->dwCurrentState == SERVICE_STOPPED)
        check(status->dwControlsAccepted == 0, "STOPPED accepts nothing");
    scm->statuses.push_back(*status);
    scm->changed.notify_all();
    return TRUE;
}

BOOL WINAPI fake_dispatch(const SERVICE_TABLE_ENTRYW* table)
{
    if (scm->dispatch_error) return FALSE;
    check(table[0].lpServiceName && table[0].lpServiceProc && !table[1].lpServiceName && !table[1].lpServiceProc, "dispatch table");
    std::jthread worker{[table]
    {
        wchar_t name[] = L"ServiceStarter 테스트";
        wchar_t* args[] = {name};
        table[0].lpServiceProc(1, args);
        std::scoped_lock lock{scm->mutex};
        scm->done = true;
        scm->changed.notify_all();
    }};
    {
        std::unique_lock lock{scm->mutex};
        check(scm->changed.wait_for(lock, 5s, [] { return scm->ready || scm->done; }), "startup timed out");
    }
    if (scm->ready && !scm->done)
    {
        check(scm->handler(SERVICE_CONTROL_INTERROGATE, 0, nullptr, scm->context) == NO_ERROR, "interrogate while running");
        check(scm->handler(SERVICE_CONTROL_PAUSE, 0, nullptr, scm->context) == ERROR_CALL_NOT_IMPLEMENTED, "pause is not implemented");
        check(scm->handler(scm->control, 0, nullptr, scm->context) == NO_ERROR, "stop control accepted");
        check(scm->handler(scm->control, 0, nullptr, scm->context) == NO_ERROR, "repeated stop is idempotent");
    }
    worker.join();
    return TRUE;
}

service_starter::windows::ScmApi api()
{
    return {&fake_dispatch, &fake_register, &fake_set_status, &fake_last_error};
}

struct Behavior
{
    bool throw_in_initialize = false;
    bool return_immediately = false;
    bool report_progress = true;         // Explicit Progress::report() during initialize.
    bool stop_callback_reports = false;  // std::stop_callback that re-enters Progress::report() (deadlock regression).
    std::chrono::milliseconds initialize_sleep{70};
    int run_result = 0;
};

std::vector<std::string> seen_args;
std::string seen_name;

class App final : public service_starter::Application
{
public:
    explicit App(Behavior behavior) : behavior_(behavior) {}
    ~App() override
    {
        // The manager may kill the process right after STOPPED: destruction must come first.
        std::scoped_lock lock{scm->mutex};
        check(scm->statuses.empty() || scm->statuses.back().dwCurrentState != SERVICE_STOPPED, "application destroyed after STOPPED was reported");
    }
    void initialize(const service_starter::HostContext& context) override
    {
        seen_args = context.args;
        seen_name = context.service_name;
        progress_ = context.progress;
        if (behavior_.report_progress) context.progress->report("step 1");
        std::this_thread::sleep_for(behavior_.initialize_sleep);
        if (behavior_.throw_in_initialize) throw std::runtime_error("init boom");
    }
    int run(std::stop_token stop) override
    {
        if (behavior_.return_immediately) return behavior_.run_result;
        std::optional<std::stop_callback<std::function<void()>>> callback;
        if (behavior_.stop_callback_reports)
            callback.emplace(stop, [this] { progress_->report("draining connections"); });
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock{mutex};
        check(changed.wait_for(lock, stop, 10s, [&stop] { return stop.stop_requested(); }), "stop never requested");
        return behavior_.run_result;
    }
    void shutdown() override {}

private:
    Behavior behavior_;
    service_starter::Progress* progress_ = nullptr;
};

service_starter::ApplicationFactory factory(Behavior behavior = {})
{
    return [behavior] { return std::make_unique<App>(behavior); };
}

service_starter::ServiceOptions options()
{
    service_starter::ServiceOptions result;
    result.name = "ServiceStarterTest";
    result.change_directory_to_executable = false;
    result.progress_interval = 25ms;
    result.logger = [](service_starter::LogLevel, std::string_view) {};
    return result;
}
}

int main()
{
    for (const auto control : {SERVICE_CONTROL_STOP, SERVICE_CONTROL_SHUTDOWN, SERVICE_CONTROL_PRESHUTDOWN})
    {
        FakeScm fake;
        scm = &fake;
        fake.control = control;
        Behavior behavior;
        behavior.stop_callback_reports = true; // Would deadlock if the control handler held the lock while requesting stop.
        const auto result = service_starter::windows::try_run_as_service(options(), factory(behavior), {"--port", "7000"}, api());
        check(result && *result == 0, "normal service run succeeds");
        check(fake.statuses.size() >= 5, "pending checkpoints, running, stop pending, stopped");
        check(fake.statuses.front().dwCurrentState == SERVICE_START_PENDING, "starts pending");
        check(fake.statuses.back().dwCurrentState == SERVICE_STOPPED && fake.statuses.back().dwWin32ExitCode == NO_ERROR, "ends STOPPED/NO_ERROR");
        std::size_t stop_pending = 0;
        for (const auto& status : fake.statuses) stop_pending += status.dwCurrentState == SERVICE_STOP_PENDING;
        check(stop_pending >= 1, "STOP_PENDING reported from the control handler");
        check(seen_args == std::vector<std::string>{"--port", "7000"}, "application args reach the service");
        check(seen_name == "ServiceStarter 테스트", "service name is UTF-8 from SCM");
    }
    {
        FakeScm fake;
        scm = &fake;
        Behavior behavior;
        behavior.throw_in_initialize = true;
        const auto result = service_starter::windows::try_run_as_service(options(), factory(behavior), {}, api());
        check(result && *result == service_starter::exit_initialize_failed, "initialize failure exit code");
        const auto& last = fake.statuses.back();
        check(last.dwCurrentState == SERVICE_STOPPED && last.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR
              && last.dwServiceSpecificExitCode == static_cast<DWORD>(service_starter::exit_initialize_failed),
              "SCM sees a service-specific failure (recovery actions can fire)");
    }
    {
        FakeScm fake;
        scm = &fake;
        Behavior behavior;
        behavior.return_immediately = true;
        behavior.run_result = 42;
        const auto result = service_starter::windows::try_run_as_service(options(), factory(behavior), {}, api());
        check(result && *result == 42 && fake.statuses.back().dwServiceSpecificExitCode == 42, "run() exit code is forwarded");
    }
    for (const auto failure : {ERROR_FAILED_SERVICE_CONTROLLER_CONNECT, ERROR_ACCESS_DENIED})
    {
        FakeScm fake;
        scm = &fake;
        fake.dispatch_error = failure;
        try
        {
            const auto result = service_starter::windows::try_run_as_service(options(), factory(), {}, api());
            check(failure == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT && !result, "1063 => console fallback (nullopt)");
        }
        catch (const std::system_error& exception)
        {
            check(failure == ERROR_ACCESS_DENIED && exception.code().value() == ERROR_ACCESS_DENIED, "other dispatcher errors throw");
        }
    }
    for (const auto failure_state : {SERVICE_START_PENDING, SERVICE_RUNNING})
    {
        // The application waits for a stop that SCM can never deliver (it was not told we are RUNNING);
        // the host must request the stop itself instead of running unmanaged.
        FakeScm fake;
        scm = &fake;
        fake.fail_state = failure_state;
        const auto result = service_starter::windows::try_run_as_service(options(), factory(), {}, api());
        check(result && *result == service_starter::exit_host_failed, "SetServiceStatus failure fails the host");
        check(fake.statuses.back().dwCurrentState == SERVICE_STOPPED && fake.statuses.back().dwWin32ExitCode == ERROR_ACCESS_DENIED, "final STOPPED carries the SCM error");
    }
    {
        // Automatic checkpoints stop after progress_timeout without an explicit report, so a hung
        // initialize() is eventually visible to SCM instead of being refreshed forever.
        FakeScm fake;
        scm = &fake;
        auto bounded = options();
        bounded.progress_interval = 20ms;
        bounded.progress_timeout = 60ms;
        Behavior behavior;
        behavior.report_progress = false;
        behavior.initialize_sleep = 400ms;
        behavior.return_immediately = true;
        check(service_starter::windows::try_run_as_service(bounded, factory(behavior), {}, api()).value_or(-1) == 0, "bounded run succeeds");
        std::size_t start_pending = 0;
        for (const auto& status : fake.statuses) start_pending += status.dwCurrentState == SERVICE_START_PENDING;
        check(start_pending >= 2 && start_pending <= 7, "automatic checkpoints are bounded by progress_timeout");
    }
    {
        FakeScm fake;
        scm = &fake;
        fake.registration_fails = true;
        const auto result = service_starter::windows::try_run_as_service(options(), factory(), {}, api());
        check(result && *result == service_starter::exit_host_failed && fake.statuses.empty(), "registration failure uses no status handle");
    }
    {
        const auto command = service_starter::windows::service_command_line(L"C:\\Program Files\\X Tank\\XTankServer.exe", {"a b", "c\"d", "plain", "end\\"});
        // A trailing backslash needs no quoting unless the argument is quoted for another reason.
        check(command == L"\"C:\\Program Files\\X Tank\\XTankServer.exe\" --service -- \"a b\" \"c\\\"d\" plain end\\", "command line quoting");
        const auto quoted = service_starter::windows::service_command_line(L"C:\\S.exe", {"tail \\", "x\\\\\"y"});
        check(quoted == L"\"C:\\S.exe\" --service -- \"tail \\\\\" \"x\\\\\\\\\\\"y\"", "backslashes before a quote or the end are doubled");
    }
    std::cout << "service_starter SCM tests: PASS\n";
    return EXIT_SUCCESS;
}
