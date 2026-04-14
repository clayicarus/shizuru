// app/tools/builtin_tools.cpp — Builtin tool registration.
// Stub implementation — to be filled in.

#include "app/tools/builtin_tools.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace shizuru::app {

void RegisterBuiltinTools(runtime::ToolRegistry& registry,
                          SchedulerDevice* /*scheduler*/) {
  registry.Register("get_current_time",
                    [](const std::string& /*args*/) -> runtime::ToolResult {
                      auto now = std::chrono::system_clock::now();
                      auto t = std::chrono::system_clock::to_time_t(now);
                      char buf[64];
                      std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                                    std::localtime(&t));
                      return {true, buf, ""};
                    });

  registry.Register("get_system_info",
                    [](const std::string& /*args*/) -> runtime::ToolResult {
                      char hostname[256] = {};
                      gethostname(hostname, sizeof(hostname));
#if defined(__APPLE__)
                      const char* os = "macOS";
#elif defined(__linux__)
                      const char* os = "Linux";
#elif defined(_WIN32)
                      const char* os = "Windows";
#else
                      const char* os = "Unknown";
#endif
                      std::string info = std::string(R"({"os":")") + os +
                                         R"(","hostname":")" + hostname + R"("})";
                      return {true, info, ""};
                    });

  // TODO: calculate, set_reminder, save_note, save_followup,
  //       list_followups, complete_followup
}

std::vector<services::ToolDefinition> BuiltinToolDefinitions() {
  std::vector<services::ToolDefinition> defs;

  {
    services::ToolDefinition d;
    d.name = "get_current_time";
    d.description = "Get the current date and time";
    d.required_capability = "builtin";
    defs.push_back(std::move(d));
  }
  {
    services::ToolDefinition d;
    d.name = "get_system_info";
    d.description = "Get system information (OS, hostname)";
    d.required_capability = "builtin";
    defs.push_back(std::move(d));
  }

  // TODO: definitions for calculate, set_reminder, save_note,
  //       save_followup, list_followups, complete_followup

  return defs;
}

}  // namespace shizuru::app
