#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "content_part.h"

namespace shizuru::core {

enum class ActorKind {
  kHuman,
  kAssistant,
  kSystem,
  kTool,
};

struct ActorRef {
  std::string actor_id;
  std::string display_name;
  ActorKind kind = ActorKind::kHuman;
};

enum class ConversationItemKind {
  kUserMessage,
  kAssistantMessage,
  kSystemEvent,
  kToolCall,
  kToolResult,
};

struct ConversationItem {
  std::string item_id;
  std::string conversation_id;
  ConversationItemKind kind = ConversationItemKind::kUserMessage;
  ActorRef actor;
  ContentParts parts;
  std::chrono::system_clock::time_point wall_time;
  std::optional<std::string> reply_to_item_id;
};

}  // namespace shizuru::core
