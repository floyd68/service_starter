// Windows-specific entry points, exposed for integration and for testing without SCM privileges.
#pragma once
#ifdef _WIN32

#include "service_starter.hpp"

#include <windows.h>

#include <optional>

namespace service_starter::windows
{
// The four SCM calls the host depends on. Replace them in tests to drive the lifecycle from a fake manager.
struct ScmApi
{
    decltype(&::StartServiceCtrlDispatcherW) dispatch = &::StartServiceCtrlDispatcherW;
    decltype(&::RegisterServiceCtrlHandlerExW) register_handler = &::RegisterServiceCtrlHandlerExW;
    decltype(&::SetServiceStatus) set_status = &::SetServiceStatus;
    decltype(&::GetLastError) last_error = &::GetLastError;
};

// Runs under SCM. nullopt means StartServiceCtrlDispatcher reported ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
// (the process was not started by SCM); any other dispatcher failure throws std::system_error.
std::optional<int> try_run_as_service(const ServiceOptions& options, const ApplicationFactory& factory,
                                      const std::vector<std::string>& app_args, ScmApi api = {});

// Command line registered with SCM: "<executable>" --service [-- app args], with CommandLineToArgvW quoting.
std::wstring service_command_line(const std::filesystem::path& executable, const std::vector<std::string>& app_args);
}

#endif
