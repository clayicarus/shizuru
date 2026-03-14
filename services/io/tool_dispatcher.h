#pragma once

#include "interfaces/io_bridge.h"
#include "io/tool_registry.h"

namespace shizuru::services {

// IoBridge implementation that dispatches ActionCandidates to registered tools.
class ToolDispatcher : public core::IoBridge {
 public:
  explicit ToolDispatcher(ToolRegistry& registry);

  core::ActionResult Execute(const core::ActionCandidate& action) override;

  void Cancel() override;

 private:
  ToolRegistry& registry_;
};

}  // namespace shizuru::services
