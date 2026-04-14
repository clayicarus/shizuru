#pragma once

// app/tools/builtin_tools.h — Register all product-level builtin tools.
//
// Centralizes tool registration that was previously scattered in
// shizuru_bridge.cpp.  Each tool is a pure function: takes JSON args,
// returns ToolResult.  Side effects (persistence, scheduling) go through
// injected dependencies.
//
// Tool list (MVP):
//   - get_current_time    — current date/time
//   - get_system_info     — OS + hostname
//   - calculate           — simple math expression
//   - set_reminder        — schedule a reminder (via SchedulerDevice)
//   - save_note           — persist a quick note
//   - save_followup       — create a followup item
//   - list_followups      — query active followups for the user
//   - complete_followup   — mark a followup as done

#include <string>
#include <vector>

#include "runtime/tool_registry.h"
#include "services/llm/config.h"  // ToolDefinition

namespace shizuru::app {

class SchedulerDevice;  // forward

// Register all builtin tool functions on the registry.
// scheduler may be nullptr if scheduling is not available.
void RegisterBuiltinTools(runtime::ToolRegistry& registry,
                          SchedulerDevice* scheduler);

// Return LLM tool definitions for all builtin tools (for function calling).
std::vector<services::ToolDefinition> BuiltinToolDefinitions();

}  // namespace shizuru::app
