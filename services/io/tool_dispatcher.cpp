#include "io/tool_dispatcher.h"

namespace shizuru::services {

ToolDispatcher::ToolDispatcher(ToolRegistry& registry)
    : registry_(registry) {}

core::ActionResult ToolDispatcher::Execute(
    const core::ActionCandidate& action) {
  const auto* fn = registry_.Find(action.action_name);
  if (!fn) {
    return core::ActionResult{
        false, "",
        "Unknown tool: " + action.action_name};
  }

  try {
    return (*fn)(action.arguments);
  } catch (const std::exception& e) {
    return core::ActionResult{
        false, "",
        "Tool execution error: " + std::string(e.what())};
  }
}

void ToolDispatcher::Cancel() {
  // Tool cancellation is not supported in the synchronous dispatch model.
  // Individual tools can implement their own cancellation if needed.
}

}  // namespace shizuru::services
