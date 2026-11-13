// service_starter: one application lifecycle for a development console process and a deployed
// system service (Windows SCM, Linux systemd). C++20, no third-party dependencies.
#pragma once

#include <chrono>
#include <concepts>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace service_starter
{
enum class RunMode
{
    console, // foreground process started by a user; stopped with Ctrl+C / SIGINT / SIGTERM
    service  // started by the service manager (SCM or systemd); stopped by the manager
};

enum class LogLevel
{
    debug,
    info,
    warning,
    error
};

using Logger = std::function<void(LogLevel, std::string_view)>;

// Progress reporting for long initialize()/shutdown() steps. Each call extends the manager's
// pending timeout (SCM checkpoint) and publishes a status line (systemd STATUS=). Thread-safe.
class Progress
{
public:
    virtual ~Progress() = default;
    virtual void report(std::string_view status = {}) = 0;
};

struct HostContext
{
    RunMode mode = RunMode::console;
    std::string service_name;         // UTF-8. Name given by SCM/unit, or ServiceOptions::name in console mode.
    std::filesystem::path executable; // Full path of the running executable.
    std::vector<std::string> args;    // Application arguments (UTF-8) after host options were removed.
    Progress* progress = nullptr;     // Valid for the whole lifecycle.
    Logger log;                       // Never empty.
};

// Lifecycle contract. The host owns the order: construct -> initialize -> run -> shutdown -> destroy.
//  - initialize: acquire resources (config, DB, listeners). Throw to fail startup.
//  - run: block until stop.stop_requested(); return the process exit code (0 = success).
//  - shutdown: release resources. Runs even when run() threw; not called when initialize() threw.
class Application
{
public:
    virtual ~Application() = default;
    virtual void initialize(const HostContext& context) = 0;
    virtual int run(std::stop_token stop) = 0;
    virtual void shutdown() = 0;
};

template <class App>
concept ServiceApplication = std::default_initializable<App> &&
    requires(App& app, const HostContext& context, std::stop_token stop) {
        { app.initialize(context) } -> std::same_as<void>;
        { app.run(stop) } -> std::convertible_to<int>;
        { app.shutdown() } -> std::same_as<void>;
    };

using ApplicationFactory = std::function<std::unique_ptr<Application>()>;

struct ServiceOptions
{
    std::string name = "Service";   // Default service/unit name (UTF-8). Also the console-mode service_name.
    std::string display_name;       // Windows display name / systemd Description. Defaults to name.
    std::string description;        // Windows service description.
    bool auto_start = false;        // Windows: SERVICE_AUTO_START. Linux: enabling the unit stays an operator step.
    bool change_directory_to_executable = true; // Applied in every mode so console and service behave alike.

    // While initialize()/shutdown() run, the host reports a checkpoint every progress_interval, but only for
    // progress_timeout after the last explicit Progress::report(). A step that stays silent longer than that
    // stops being covered, so the manager's own timeout can detect a hang instead of being fed forever.
    std::chrono::milliseconds progress_interval{1000};
    std::chrono::milliseconds progress_timeout{60000};
    std::chrono::seconds stop_timeout{30};             // systemd TimeoutStopSec; SCM stop wait hint.
    std::chrono::seconds restart_delay{5};             // Restart delay registered with the manager.
    std::chrono::seconds watchdog{0};                  // >0 writes WatchdogSec. WATCHDOG_USEC is honored regardless.

    // Windows
    std::string account = "NT AUTHORITY\\LocalService"; // Empty = LocalSystem.
    std::vector<std::string> dependencies;              // Service names this service depends on.

    // Linux / systemd
    std::string unit_user;  // Empty = not written (runs as root unless the operator edits the unit).
    std::string unit_group; // Empty = same as unit_user.
    std::filesystem::path unit_directory = "/etc/systemd/system";
    bool unit_hardening = true; // NoNewPrivileges, PrivateTmp, ProtectSystem=strict, ProtectHome.

    Logger logger; // Empty = platform default (console: stdout/stderr; SCM: Event Log; systemd: journal prefixes).
};

// Process exit codes produced by the host itself. An application's run() return value is passed through.
inline constexpr int exit_usage = 2;
inline constexpr int exit_initialize_failed = 10;
inline constexpr int exit_run_failed = 11;
inline constexpr int exit_shutdown_failed = 12;
inline constexpr int exit_host_failed = 13;

// Entry point. Host command line:
//   <exe>                         auto: service when launched by the manager, otherwise console
//   <exe> --console               force console mode
//   <exe> --service               force service mode (fails when not launched by SCM on Windows)
//   <exe> install [NAME]          register the service (Windows SCM / systemd unit file)
//   <exe> uninstall [NAME]        unregister the service
//   <exe> --help
// Everything after "--" is passed to the application unchanged and is stored with an installed service.
int run_main(int argc, char** argv, const ServiceOptions& options, ApplicationFactory factory);
#ifdef _WIN32
int run_main(int argc, wchar_t** argv, const ServiceOptions& options, ApplicationFactory factory);
#endif

namespace detail
{
template <ServiceApplication App> class Adapter final : public Application
{
public:
    void initialize(const HostContext& context) override { app_.initialize(context); }
    int run(std::stop_token stop) override { return static_cast<int>(app_.run(std::move(stop))); }
    void shutdown() override { app_.shutdown(); }

private:
    App app_{};
};
}

template <ServiceApplication App, class Char>
int run_main(int argc, Char** argv, const ServiceOptions& options = {})
{
    return run_main(argc, argv, options, [] { return std::unique_ptr<Application>(new detail::Adapter<App>()); });
}

// Runs the lifecycle in the current process without consulting the service manager.
// Windows accepts RunMode::console only (service mode must come from SCM through run_main).
int run_foreground(const ServiceOptions& options, ApplicationFactory factory, std::vector<std::string> app_args,
                   RunMode mode = RunMode::console);

// Requests the active host to stop, as if the manager had asked. Returns false when nothing is running.
bool request_stop() noexcept;

std::string help_text(const ServiceOptions& options, std::string_view program);

// systemd unit text that install writes on Linux. Pure function, available on every platform.
std::string systemd_unit_text(const ServiceOptions& options, const std::filesystem::path& executable,
                              const std::vector<std::string>& app_args);
}
