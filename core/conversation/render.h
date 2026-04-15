#pragma once

#include "context/types.h"
#include "conversation/item.h"

namespace shizuru::core::conversation {

ContextMessage RenderForLlm(const ConversationItem& item);

}  // namespace shizuru::core::conversation
