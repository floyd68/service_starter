// Minimal application hosted by service_starter. Logs a heartbeat until the host asks it to stop.
//   example_service --console -- --period 500            run in the foreground
//   example_service --console -- --exit-after 2000       stop itself after 2 s (used by smoke tests)
//   example_service install [NAME] -- --period 1000      register with the service manager
#include "service_starter/service_starter.hpp"

#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

namespace
{
class ExampleService
{
public:
    void initialize(const service_starter::HostContext& context)
    {
        log_ = context.log;
        for (std::size_t i = 0; i < context.args.size(); ++i)
        {
            const auto& argument = context.args[i];
            if ((argument == "--period" || argument == "--exit-after") && i + 1 < context.args.size())
            {
                int value = 0;
                const auto& text = context.args[++i];
                if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{} || value < 0)
                    throw std::invalid_argument("invalid number for " + argument + ": " + text);
                (argument == "--period" ? period_ : exit_after_) = std::chrono::milliseconds{value};
            }
            else if (argument == "--fail")
                throw std::runtime_error("startup failure requested by --fail");
            else
                throw std::invalid_argument("unknown application argument: " + argument);
        }
        context.progress->report("configuration loaded");
        log_(service_starter::LogLevel::info, "mode=" + std::string{context.mode == service_starter::RunMode::service ? "service" : "console"}
                                                  + " name=" + context.service_name + " exe=" + context.executable.string());
    }

    int run(std::stop_token stop)
    {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock{mutex};
        const auto started = std::chrono::steady_clock::now();
        int beats = 0;
        while (!changed.wait_for(lock, stop, period_, [&stop] { return stop.stop_requested(); }))
        {
            log_(service_starter::LogLevel::info, "heartbeat " + std::to_string(++beats));
            if (exit_after_.count() > 0 && std::chrono::steady_clock::now() - started >= exit_after_)
            {
                log_(service_starter::LogLevel::info, "exit-after reached; stopping");
                service_starter::request_stop();
            }
        }
        return 0;
    }

    void shutdown()
    {
        log_(service_starter::LogLevel::info, "resources released");
    }

private:
    service_starter::Logger log_;
    std::chrono::milliseconds period_{1000};
    std::chrono::milliseconds exit_after_{0};
};

service_starter::ServiceOptions options()
{
    service_starter::ServiceOptions result;
    result.name = "ServiceStarterExample";
    result.display_name = "service_starter example";
    result.description = "Heartbeat service hosted by service_starter";
    result.unit_user = "nobody";
    if (const char* unit_dir = std::getenv("SERVICE_STARTER_UNIT_DIR")) // Lets non-root smoke tests write the unit file.
        result.unit_directory = unit_dir;
    return result;
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    return service_starter::run_main<ExampleService>(argc, argv, options());
}
#else
int main(int argc, char** argv)
{
    return service_starter::run_main<ExampleService>(argc, argv, options());
}
#endif
