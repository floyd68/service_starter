// Internal interfaces shared by the common lifecycle and the platform hosts.
#pragma once

#include "service_starter/service_starter.hpp"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace service_starter::detail
{
// Where lifecycle transitions are published: console log, SCM status, systemd notifications.
class StatusSink
{
public:
    virtual ~StatusSink() = default;
    virtual void starting() = 0;
    virtual void progress(std::string_view status) = 0; // Pending checkpoint and/or status text.
    virtual void running() = 0;
    virtual void stopping() = 0;
    virtual void stopped(int exit_code, std::string_view failure) = 0;
};

// Console mode: the lifecycle logs transitions itself, so nothing else is published.
class NullSink final : public StatusSink
{
public:
    void starting() override {}
    void progress(std::string_view) override {}
    void running() override {}
    void stopping() override {}
    void stopped(int, std::string_view) override {}
};

enum class Command
{
    run_auto,
    run_console,
    run_service,
    install,
    uninstall,
    help,
    invalid
};

struct ParsedCommandLine
{
    Command command = Command::run_auto;
    std::string name;                  // install/uninstall: optional service name.
    std::vector<std::string> app_args; // Everything after "--".
    std::string error;                 // Set when command == invalid.
};

ParsedCommandLine parse_command_line(const std::vector<std::string>& args);

// Runs construct -> initialize -> run -> shutdown -> destroy with pending progress ticks.
// Returns the process exit code. Never throws; failures are logged and reported to the sink.
// The stop source is taken by value: copies share the stop state, so a late request_stop() from another
// thread never touches a dead object.
int run_lifecycle(const ServiceOptions& options, HostContext& context, const ApplicationFactory& factory,
                  StatusSink& sink, std::stop_source stop);

// Registry for request_stop(). Only one host runs per process.
class ActiveStop
{
public:
    explicit ActiveStop(std::stop_source source) noexcept;
    ~ActiveStop();
    ActiveStop(const ActiveStop&) = delete;
    ActiveStop& operator=(const ActiveStop&) = delete;
    static bool request() noexcept;
};

// Wraps a logger so that a throwing logger can never derail the lifecycle.
Logger make_safe_logger(Logger logger);

// Shared by the char and wchar_t entry points once arguments are UTF-8.
int run_main_impl(std::vector<std::string> args, const std::string& program, const ServiceOptions& options,
                  const ApplicationFactory& factory);

std::string describe_exception();           // Call inside a catch block.
Logger resolve_logger(const ServiceOptions& options, RunMode mode);
std::string display_name(const ServiceOptions& options);
std::string quote_for_display(std::string_view argument);

// Platform-specific pieces implemented in host_windows.cpp / host_posix.cpp.
Logger default_logger(RunMode mode, std::string_view service_name);
std::filesystem::path executable_path();
int install_service(const ServiceOptions& options, const std::string& name, const std::vector<std::string>& app_args);
int uninstall_service(const ServiceOptions& options, const std::string& name);
// Windows: tries the SCM dispatcher; nullopt means "not launched by SCM" (console fallback allowed).
// Linux: returns nullopt when no service manager environment is detected.
std::optional<int> run_as_service(const ServiceOptions& options, const ApplicationFactory& factory,
                                  const std::vector<std::string>& app_args, bool require_manager);
}
