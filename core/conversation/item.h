#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace shizuru::core::conversation {

enum class ActorKind {
  kHuman,
  kAssistant,
  kSystem,
  kTool,
};

enum class ItemKind {
  kHumanMessage,
  kAssistantMessage,
  kToolCall,
  kSystemEvent,
  kToolResult,
};

struct Actor {
  std::string actor_id;
  ActorKind kind = ActorKind::kHuman;
  std::string display_name;
};

struct ConversationItem {
  std::string item_id;
  std::string conversation_id;
  ItemKind kind = ItemKind::kHumanMessage;
  Actor actor;
  std::optional<std::string> reply_to_item_id;
  std::vector<std::string> mentions;
  std::optional<std::string> turn_group_id;
  nlohmann::json payload = nlohmann::json::object();
};

const char* ActorKindName(ActorKind kind);
const char* ItemKindName(ItemKind kind);

ActorKind ActorKindFromString(const std::string& value);
ItemKind ItemKindFromString(const std::string& value);

ConversationItem MakeHumanMessageItem(std::string actor_id,
                                      std::string actor_name,
                                      std::string text);

ConversationItem MakeAssistantMessageItem(std::string actor_id,
                                          std::string actor_name,
                                          std::string text);

ConversationItem MakeToolCallItem(std::string actor_id,
                                  std::string actor_name,
                                  nlohmann::json tool_calls);

ConversationItem MakeSystemEventItem(std::string actor_id,
                                     std::string actor_name,
                                     std::string event_type,
                                     std::string source,
                                     nlohmann::json data);

ConversationItem MakeToolResultItem(std::string tool_name,
                                    std::string tool_call_id,
                                    nlohmann::json content);

nlohmann::json ParseJsonOrString(const std::string& text);

nlohmann::json ToJson(const ConversationItem& item);
ConversationItem FromJson(const nlohmann::json& json);

std::string SerializeConversationItem(const ConversationItem& item);
std::optional<ConversationItem> TryParseConversationItem(
    const std::string& serialized);

}  // namespace shizuru::core::conversation
