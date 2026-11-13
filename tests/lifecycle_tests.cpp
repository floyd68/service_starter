// Cross-platform lifecycle tests: ordering, failure codes, stop requests, argument parsing, unit text.
// Linux additionally covers SIGTERM delivery and systemd notifications through a fake NOTIFY_SOCKET.
#include "service_starter/service_starter.hpp"

#include "../src/host.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#endif

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

struct Trace
{
    std::mutex mutex;
    std::condition_variable_any changed;
    std::vector<std::string> events;
    std::vector<std::string> logs;
    service_starter::HostContext context;
    bool running = false;

    void event(std::string name)
    {
        std::scoped_lock lock{mutex};
        events.push_back(std::move(name));
    }
    void wait_running()
    {
        std::unique_lock lock{mutex};
        check(changed.wait_for(lock, 5s, [this] { return running; }), "application did not reach run()");
    }
};

struct Behavior
{
    bool throw_in_initialize = false;
    bool throw_in_run = false;
    bool throw_in_shutdown = false;
    bool return_immediately = false;
    int run_result = 0;
    std::function<void()> on_running;
};

class FakeApp final : public service_starter::Application
{
public:
    FakeApp(Trace& trace, Behavior behavior) : trace_(trace), behavior_(std::move(behavior)) {}

    void initialize(const service_starter::HostContext& context) override
    {
        trace_.context = context;
        trace_.event("initialize");
        context.progress->report("warming up");
        if (behavior_.throw_in_initialize) throw std::runtime_error("init boom");
    }

    int run(std::stop_token stop) override
    {
        trace_.event("run");
        {
            std::scoped_lock lock{trace_.mutex};
            trace_.running = true;
        }
        trace_.changed.notify_all();
        if (behavior_.on_running) behavior_.on_running();
        if (behavior_.throw_in_run) throw std::runtime_error("run boom");
        if (!behavior_.return_immediately)
        {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::unique_lock lock{mutex};
            check(changed.wait_for(lock, stop, 10s, [&stop] { return stop.stop_requested(); }), "stop was never requested");
        }
        return behavior_.run_result;
    }

    void shutdown() override
    {
        trace_.event("shutdown");
        if (behavior_.throw_in_shutdown) throw std::runtime_error("shutdown boom");
    }

private:
    Trace& trace_;
    Behavior behavior_;
};

service_starter::ServiceOptions quiet_options(Trace& trace)
{
    service_starter::ServiceOptions options;
    options.name = "ServiceStarterTest";
    options.change_directory_to_executable = false;
    options.progress_interval = 20ms;
    options.logger = [&trace](service_starter::LogLevel, std::string_view message)
    {
        std::scoped_lock lock{trace.mutex};
        trace.logs.emplace_back(message);
    };
    return options;
}

service_starter::ApplicationFactory factory(Trace& trace, Behavior behavior = {})
{
    return [&trace, behavior] { return std::make_unique<FakeApp>(trace, behavior); };
}

bool has_event(const Trace& trace, std::string_view name)
{
    for (const auto& event : trace.events)
        if (event == name) return true;
    return false;
}

void test_parse()
{
    using namespace service_starter::detail;
    check(parse_command_line({}).command == Command::run_auto, "empty => auto");
    check(parse_command_line({"--console"}).command == Command::run_console, "--console");
    check(parse_command_line({"--service"}).command == Command::run_service, "--service");
    check(parse_command_line({"--help"}).command == Command::help && parse_command_line({"/?"}).command == Command::help, "help");
    auto install = parse_command_line({"install", "My Svc", "--", "--port", "7000"});
    check(install.command == Command::install && install.name == "My Svc" && install.app_args == std::vector<std::string>{"--port", "7000"}, "install NAME -- args");
    auto uninstall = parse_command_line({"uninstall"});
    check(uninstall.command == Command::uninstall && uninstall.name.empty(), "uninstall default name");
    check(parse_command_line({"--console", "--", "x"}).app_args == std::vector<std::string>{"x"}, "args after --");
    check(parse_command_line({"--port"}).command == Command::invalid, "unknown token is invalid");
    check(parse_command_line({"--console", "--service"}).command == Command::invalid, "two commands are invalid");
    check(parse_command_line({"install", "--", "a"}).name.empty(), "install does not eat --");
}

void test_console_stop_request()
{
    Trace trace;
    const auto options = quiet_options(trace);
    int result = -1;
    std::jthread worker{[&] { result = service_starter::run_foreground(options, factory(trace), {"alpha", "beta gamma"}); }};
    trace.wait_running();
    check(service_starter::request_stop(), "request_stop must find the active host");
    worker.join();
    check(result == 0, "normal run exits 0");
    check(trace.events == std::vector<std::string>{"initialize", "run", "shutdown"}, "lifecycle order");
    check(trace.context.mode == service_starter::RunMode::console, "console mode in context");
    check(trace.context.service_name == "ServiceStarterTest", "service name from options");
    check(trace.context.args == std::vector<std::string>{"alpha", "beta gamma"}, "application args pass through");
    check(!trace.context.executable.empty(), "executable path is resolved");
    check(!service_starter::request_stop(), "no active host after the run");
}

void test_failures()
{
    {
        Trace trace;
        Behavior behavior;
        behavior.throw_in_initialize = true;
        const auto result = service_starter::run_foreground(quiet_options(trace), factory(trace, behavior), {});
        check(result == service_starter::exit_initialize_failed, "initialize failure code");
        check(!has_event(trace, "run") && !has_event(trace, "shutdown"), "no run/shutdown after failed initialize");
    }
    {
        Trace trace;
        Behavior behavior;
        behavior.throw_in_run = true;
        const auto result = service_starter::run_foreground(quiet_options(trace), factory(trace, behavior), {});
        check(result == service_starter::exit_run_failed && has_event(trace, "shutdown"), "run failure still shuts down");
    }
    {
        Trace trace;
        Behavior behavior;
        behavior.return_immediately = true;
        behavior.run_result = 7;
        check(service_starter::run_foreground(quiet_options(trace), factory(trace, behavior), {}) == 7, "run result is the exit code");
    }
    {
        Trace trace;
        Behavior behavior;
        behavior.return_immediately = true;
        behavior.throw_in_shutdown = true;
        check(service_starter::run_foreground(quiet_options(trace), factory(trace, behavior), {}) == service_starter::exit_shutdown_failed, "shutdown failure code");
    }
}

void test_throwing_logger()
{
    Trace trace;
    auto options = quiet_options(trace);
    options.logger = [&trace](service_starter::LogLevel, std::string_view message)
    {
        if (message == "Running" || message.rfind("Stopped", 0) == 0) throw std::runtime_error("logger boom");
        std::scoped_lock lock{trace.mutex};
        trace.logs.emplace_back(message);
    };
    Behavior behavior;
    behavior.return_immediately = true;
    const auto result = service_starter::run_foreground(options, factory(trace, behavior), {});
    check(result == 0, "a throwing logger does not change the exit code");
    check(trace.events == std::vector<std::string>{"initialize", "run", "shutdown"}, "a throwing logger does not skip shutdown");
}

void test_run_main()
{
    Trace trace;
    Behavior behavior;
    behavior.return_immediately = true;
    const auto options = quiet_options(trace);
    {
        char program[] = "tester";
        char console[] = "--console";
        char separator[] = "--";
        char argument[] = "--port=7";
        char* argv[] = {program, console, separator, argument, nullptr};
        check(service_starter::run_main(4, argv, options, factory(trace, behavior)) == 0, "run_main --console");
        check(trace.context.args == std::vector<std::string>{"--port=7"}, "run_main forwards application args");
    }
    {
        char program[] = "tester";
        char bogus[] = "--bogus";
        char* argv[] = {program, bogus, nullptr};
        check(service_starter::run_main(2, argv, options, factory(trace, behavior)) == service_starter::exit_usage, "unknown argument => usage");
    }
    check(!service_starter::help_text(options, "tester").empty(), "help text");
}

struct TemplateApp
{
    static inline std::atomic<int> calls{0};
    void initialize(const service_starter::HostContext&) { ++calls; }
    int run(std::stop_token) { ++calls; return 0; }
    void shutdown() { ++calls; }
};
static_assert(service_starter::ServiceApplication<TemplateApp>);

void test_template_entry()
{
    Trace trace;
    char program[] = "tester";
    char console[] = "--console";
    char* argv[] = {program, console, nullptr};
    check(service_starter::run_main<TemplateApp>(2, argv, quiet_options(trace)) == 0, "template run_main");
    check(TemplateApp::calls == 3, "template adapter forwards all three calls");
}

void test_unit_text()
{
    service_starter::ServiceOptions options;
    options.name = "xtank-server";
    options.display_name = "XTank server";
    options.unit_user = "xtank";
    options.watchdog = 30s;
    const auto text = service_starter::systemd_unit_text(options, "/opt/x tank/bin/XTankServer", {"--port", "70$0", "a b"});
    check(text.find("Type=notify\n") != text.npos, "Type=notify");
    check(text.find("ExecStart=\"/opt/x tank/bin/XTankServer\" --service -- --port \"70$$0\" \"a b\"\n") != text.npos, "ExecStart quoting");
    check(text.find("WorkingDirectory=\"/opt/x tank/bin\"\n") != text.npos, "WorkingDirectory");
    check(text.find("User=xtank\nGroup=xtank\n") != text.npos, "User/Group");
    check(text.find("WatchdogSec=30\n") != text.npos && text.find("TimeoutStopSec=30\n") != text.npos, "timeouts");
    check(text.find("ProtectSystem=strict\n") != text.npos, "hardening");
    check(text.find("Description=XTank server\n") != text.npos, "description");
}

#ifndef _WIN32
void test_sigterm()
{
    Trace trace;
    Behavior behavior;
    behavior.on_running = [] { ::kill(::getpid(), SIGTERM); };
    const auto result = service_starter::run_foreground(quiet_options(trace), factory(trace, behavior), {});
    check(result == 0, "SIGTERM stops the console run cleanly");
    bool logged = false;
    for (const auto& line : trace.logs) logged |= line.find("SIGTERM") != line.npos;
    check(logged, "signal is logged");
}

void test_systemd_notify()
{
    char directory[] = "/tmp/service_starter_XXXXXX";
    check(::mkdtemp(directory) != nullptr, "mkdtemp");
    const std::string socket_path = std::string{directory} + "/notify";
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    check(fd >= 0, "socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    check(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
    ::setenv("NOTIFY_SOCKET", socket_path.c_str(), 1);
    ::setenv("WATCHDOG_USEC", "100000", 1);
    ::unsetenv("WATCHDOG_PID");

    Trace trace;
    Behavior behavior;
    behavior.on_running = []
    {
        std::this_thread::sleep_for(250ms); // Let a few WATCHDOG pings go out.
        service_starter::request_stop();
    };
    const auto result = service_starter::run_foreground(quiet_options(trace), factory(trace, behavior), {}, service_starter::RunMode::service);
    check(result == 0, "service-mode run exits 0");
    check(trace.context.mode == service_starter::RunMode::service, "service mode in context");

    std::string received;
    char buffer[512];
    for (;;)
    {
        const auto size = ::recv(fd, buffer, sizeof(buffer), 0);
        if (size <= 0) break;
        received.append(buffer, static_cast<std::size_t>(size));
        received += '|';
    }
    check(received.find("STATUS=warming up") != received.npos, "progress status forwarded");
    check(received.find("EXTEND_TIMEOUT_USEC=") != received.npos && received.find("EXTEND_TIMEOUT_USEC=") < received.find("READY=1"),
          "pending progress extends the systemd start timeout");
    check(received.find("READY=1") != received.npos, "READY=1 sent");
    check(received.find("STOPPING=1") != received.npos, "STOPPING=1 sent");
    check(received.find("WATCHDOG=1") != received.npos, "watchdog pings sent");
    check(received.find("READY=1") < received.find("STOPPING=1"), "READY before STOPPING");

    ::unsetenv("NOTIFY_SOCKET");
    ::unsetenv("WATCHDOG_USEC");
    ::close(fd);
    ::unlink(socket_path.c_str());
    ::rmdir(directory);

    const std::vector<std::string> no_args;
    check(!service_starter::detail::run_as_service(quiet_options(trace), factory(trace), no_args, false).has_value(),
          "no manager environment => console fallback");
}
#endif
}

int main()
{
    test_parse();
    test_console_stop_request();
    test_failures();
    test_throwing_logger();
    test_run_main();
    test_template_entry();
    test_unit_text();
#ifndef _WIN32
    test_sigterm();
    test_systemd_notify();
#endif
    std::cout << "service_starter lifecycle tests: PASS\n";
    return EXIT_SUCCESS;
}
