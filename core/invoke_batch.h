#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "conversation_item.h"

namespace shizuru::core {

enum class TriggerReason {
  kUserFlush,
  kDebounceTimeout,
  kToolContinuation,
  kSystemEvent,
  kIdlePrompt,
};

struct InvokeBatch {
  std::string conversation_id;
  std::vector<ConversationItem> items;
  TriggerReason reason = TriggerReason::kUserFlush;
  std::chrono::steady_clock::time_point created_at;
};

}  // namespace shizuru::core
