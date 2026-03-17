#include "io/tool_dispatcher.h"

#include "async_logger.h"

namespace shizuru::services {

ToolDispatcher::ToolDispatcher(ToolRegistry& registry)
    : registry_(registry) {}

core::ActionResult ToolDispatcher::Execute(
    const core::ActionCandidate& action) {
  const auto* fn = registry_.Find(action.action_name);
  if (!fn) {
    LOG_WARN("[{}] Unknown tool: \"{}\"", MODULE_NAME, action.action_name);
    return core::ActionResult{
        false, "",
        "Unknown tool: " + action.action_name};
  }

  LOG_DEBUG("[{}] Executing tool=\"{}\" args={}",
            MODULE_NAME, action.action_name, action.arguments);
  try {
    auto result = (*fn)(action.arguments);
    if (result.success) {
      LOG_DEBUG("[{}] Tool \"{}\" succeeded: {}",
                MODULE_NAME, action.action_name, result.output);
    } else {
      LOG_WARN("[{}] Tool \"{}\" returned failure: {}",
               MODULE_NAME, action.action_name, result.error_message);
    }
    return result;
  } catch (const std::exception& e) {
    LOG_ERROR("[{}] Tool \"{}\" threw exception: {}",
              MODULE_NAME, action.action_name, e.what());
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
