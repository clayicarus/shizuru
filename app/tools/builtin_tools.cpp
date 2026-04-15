// app/tools/builtin_tools.cpp — Builtin tool registration.

#include "app/tools/builtin_tools.h"

#include "app/scheduler/scheduler_device.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace shizuru::app {

void RegisterBuiltinTools(runtime::ToolRegistry& registry,
                          SchedulerDevice* scheduler) {
  registry.Register("get_current_time",
                    [](const nlohmann::json& /*args*/) -> runtime::ToolResult {
                      auto now = std::chrono::system_clock::now();
                      auto t = std::chrono::system_clock::to_time_t(now);
                      char buf[64];
                      std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                                    std::localtime(&t));
                      return {true, buf, ""};
                    });

  registry.Register("get_system_info",
                    [](const nlohmann::json& /*args*/) -> runtime::ToolResult {
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
                      nlohmann::json info = {
                          {"os", os},
                          {"hostname", hostname},
                      };
                      return {true, std::move(info), ""};
                    });

  registry.Register("calculate",
                    [](const nlohmann::json& args) -> runtime::ToolResult {
                      if (!args.contains("expression") ||
                          !args["expression"].is_string()) {
                        return {false, "", "Missing 'expression' parameter"};
                      }
                      std::string expr = args["expression"].get<std::string>();
                      double a = 0, b = 0;
                      char op = 0;
                      if (std::sscanf(expr.c_str(), "%lf %c %lf", &a, &op, &b) != 3) {
                        return {false, "", "Cannot parse expression: " + expr};
                      }
                      double result = 0;
                      switch (op) {
                        case '+': result = a + b; break;
                        case '-': result = a - b; break;
                        case '*': result = a * b; break;
                        case '/':
                          if (b == 0) { return {false, "", "Division by zero"}; }
                          result = a / b;
                          break;
                        default:
                          return {false, "", std::string("Unknown operator: ") + op};
                      }
                      char buf[64];
                      std::snprintf(buf, sizeof(buf), "%.6g", result);
                      return {true, buf, ""};
                    });

  // set_reminder: schedule a real reminder via SchedulerDevice.
  // If scheduler is null, the tool still works but returns a "not available" message.
  registry.Register("set_reminder",
                    [scheduler](const nlohmann::json& args) -> runtime::ToolResult {
                      std::string message = "reminder";
                      int minutes = 0;

                      if (args.contains("message") && args["message"].is_string()) {
                        message = args["message"].get<std::string>();
                      }
                      if (args.contains("minutes") && args["minutes"].is_number_integer()) {
                        minutes = args["minutes"].get<int>();
                      }

                      if (scheduler == nullptr) {
                        return {false, "", "Scheduler not available"};
                      }

                      // Generate a unique ID for this reminder.
                      auto now = std::chrono::system_clock::now();
                      auto id = "reminder-" + std::to_string(
                          now.time_since_epoch().count());

                      // Build the payload — pure structured data.
                      // CoreDevice wraps it in <event> tags; persona prompt
                      // tells the LLM how to handle reminder events.
                      nlohmann::json payload = {
                          {"message", message},
                      };
                      // Calculate trigger time.
                      auto trigger = now + std::chrono::minutes(minutes);

                      ScheduledItem item;
                      item.id = id;
                      item.payload = payload.dump();
                      item.trigger_time = trigger;

                      scheduler->Schedule(std::move(item));

                      // Return confirmation to the LLM.
                      nlohmann::json result = {
                          {"status", "scheduled"},
                          {"id", id},
                          {"message", message},
                          {"minutes", minutes},
                      };
                      return {true, std::move(result), ""};
                    });

  // save_note: persist a quick note in the conversation context.
  // Currently stored in-memory (visible to LLM in current session).
  // Will be persisted to SQLite when SqliteMemoryStore is implemented.
  registry.Register("save_note",
                    [](const nlohmann::json& args) -> runtime::ToolResult {
                      if (!args.contains("content") || !args["content"].is_string()) {
                        return {false, "", "Missing 'content' parameter"};
                      }
                      std::string content = args["content"].get<std::string>();

                      // The note is returned as the tool result, which gets
                      // recorded in ContextStrategy as a tool_result message.
                      // This means the LLM will see it in subsequent turns.
                      nlohmann::json result = {
                          {"status", "saved"},
                          {"content", content},
                      };
                      return {true, std::move(result), ""};
                    });
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
  {
    services::ToolDefinition d;
    d.name = "calculate";
    d.description = "Evaluate a simple math expression (a op b)";
    d.parameters = {
        {"expression", "string", "Math expression like '2 + 3' or '10 / 4'", true}};
    d.required_capability = "builtin";
    defs.push_back(std::move(d));
  }
  {
    services::ToolDefinition d;
    d.name = "set_reminder";
    d.description = "Set a reminder that will fire after the specified number of minutes. "
                    "When the reminder fires, Shizuru will proactively bring it up.";
    d.parameters = {
        {"message", "string", "What to remind about", true},
        {"minutes", "integer", "Minutes from now until the reminder fires", true}};
    d.required_capability = "builtin";
    defs.push_back(std::move(d));
  }
  {
    services::ToolDefinition d;
    d.name = "save_note";
    d.description = "Save a quick note for the user. The note will be visible "
                    "in the conversation context so you can reference it later.";
    d.parameters = {
        {"content", "string", "The note content to save", true}};
    d.required_capability = "builtin";
    defs.push_back(std::move(d));
  }

  return defs;
}

}  // namespace shizuru::app
