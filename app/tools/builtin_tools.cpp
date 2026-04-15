// app/tools/builtin_tools.cpp — Builtin tool registration.

#include "app/tools/builtin_tools.h"

#include "app/scheduler/scheduler_device.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

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

  registry.Register("calculate",
                    [](const std::string& args) -> runtime::ToolResult {
                      auto expr_pos = args.find(R"("expression":")");
                      if (expr_pos == std::string::npos) {
                        return {false, "", "Missing 'expression' parameter"};
                      }
                      auto val_start = expr_pos + 15;
                      auto val_end = args.find('"', val_start);
                      if (val_end == std::string::npos) {
                        return {false, "", "Malformed expression"};
                      }
                      std::string expr = args.substr(val_start, val_end - val_start);
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
                    [scheduler](const std::string& args) -> runtime::ToolResult {
                      // Parse "message" and "minutes" from JSON args.
                      std::string message = "reminder";
                      int minutes = 0;

                      auto msg_pos = args.find(R"("message":")");
                      if (msg_pos != std::string::npos) {
                        auto s = msg_pos + 11;
                        auto e = args.find('"', s);
                        if (e != std::string::npos) { message = args.substr(s, e - s); }
                      }
                      auto min_pos = args.find(R"("minutes":)");
                      if (min_pos != std::string::npos) {
                        std::sscanf(args.c_str() + min_pos + 10, "%d", &minutes);
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
                      std::string payload =
                          R"({"message":")" + message + R"("})";
                      // Calculate trigger time.
                      auto trigger = now + std::chrono::minutes(minutes);

                      ScheduledItem item;
                      item.id = id;
                      item.payload = payload;
                      item.trigger_time = trigger;

                      scheduler->Schedule(std::move(item));

                      // Return confirmation to the LLM.
                      std::string result =
                          R"({"status":"scheduled","id":")" + id +
                          R"(","message":")" + message +
                          R"(","minutes":)" + std::to_string(minutes) + "}";
                      return {true, result, ""};
                    });

  // save_note: persist a quick note in the conversation context.
  // Currently stored in-memory (visible to LLM in current session).
  // Will be persisted to SQLite when SqliteMemoryStore is implemented.
  registry.Register("save_note",
                    [](const std::string& args) -> runtime::ToolResult {
                      auto content_pos = args.find(R"("content":")");
                      if (content_pos == std::string::npos) {
                        return {false, "", "Missing 'content' parameter"};
                      }
                      auto s = content_pos + 11;
                      auto e = args.find('"', s);
                      if (e == std::string::npos) {
                        return {false, "", "Malformed content"};
                      }
                      std::string content = args.substr(s, e - s);

                      // The note is returned as the tool result, which gets
                      // recorded in ContextStrategy as a tool_result message.
                      // This means the LLM will see it in subsequent turns.
                      std::string result =
                          R"({"status":"saved","content":")" + content + R"("})";
                      return {true, result, ""};
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
