#include "conversation/item.h"

#include <stdexcept>
#include <utility>

namespace shizuru::core::conversation {

const char* ActorKindName(ActorKind kind) {
  switch (kind) {
    case ActorKind::kHuman: return "human";
    case ActorKind::kAssistant: return "assistant";
    case ActorKind::kSystem: return "system";
    case ActorKind::kTool: return "tool";
    default: return "unknown";
  }
}

const char* ItemKindName(ItemKind kind) {
  switch (kind) {
    case ItemKind::kHumanMessage: return "human_message";
    case ItemKind::kAssistantMessage: return "assistant_message";
    case ItemKind::kSystemEvent: return "system_event";
    case ItemKind::kToolResult: return "tool_result";
    default: return "unknown";
  }
}

ActorKind ActorKindFromString(const std::string& value) {
  if (value == "human") { return ActorKind::kHuman; }
  if (value == "assistant") { return ActorKind::kAssistant; }
  if (value == "system") { return ActorKind::kSystem; }
  if (value == "tool") { return ActorKind::kTool; }
  throw std::invalid_argument("Unknown ActorKind: " + value);
}

ItemKind ItemKindFromString(const std::string& value) {
  if (value == "human_message") { return ItemKind::kHumanMessage; }
  if (value == "assistant_message") { return ItemKind::kAssistantMessage; }
  if (value == "system_event") { return ItemKind::kSystemEvent; }
  if (value == "tool_result") { return ItemKind::kToolResult; }
  throw std::invalid_argument("Unknown ItemKind: " + value);
}

ConversationItem MakeHumanMessageItem(std::string actor_id,
                                      std::string actor_name,
                                      std::string text) {
  ConversationItem item;
  item.kind = ItemKind::kHumanMessage;
  item.actor = Actor{std::move(actor_id), ActorKind::kHuman,
                     std::move(actor_name)};
  item.payload["text"] = std::move(text);
  return item;
}

ConversationItem MakeAssistantMessageItem(std::string actor_id,
                                          std::string actor_name,
                                          std::string text) {
  ConversationItem item;
  item.kind = ItemKind::kAssistantMessage;
  item.actor = Actor{std::move(actor_id), ActorKind::kAssistant,
                     std::move(actor_name)};
  item.payload["text"] = std::move(text);
  return item;
}

ConversationItem MakeSystemEventItem(std::string actor_id,
                                     std::string actor_name,
                                     std::string event_type,
                                     std::string source,
                                     nlohmann::json data) {
  ConversationItem item;
  item.kind = ItemKind::kSystemEvent;
  item.actor = Actor{std::move(actor_id), ActorKind::kSystem,
                     std::move(actor_name)};
  item.payload["event_type"] = std::move(event_type);
  item.payload["source"] = std::move(source);
  item.payload["data"] = std::move(data);
  return item;
}

ConversationItem MakeToolResultItem(std::string tool_name,
                                    std::string tool_call_id,
                                    nlohmann::json content) {
  ConversationItem item;
  item.kind = ItemKind::kToolResult;
  item.actor = Actor{"tool:" + tool_name, ActorKind::kTool, tool_name};
  item.payload["tool_name"] = std::move(tool_name);
  item.payload["tool_call_id"] = std::move(tool_call_id);
  item.payload["content"] = std::move(content);
  return item;
}

nlohmann::json ParseJsonOrString(const std::string& text) {
  try {
    return nlohmann::json::parse(text);
  } catch (...) {
    return text;
  }
}

nlohmann::json ToJson(const ConversationItem& item) {
  nlohmann::json json;
  json["item_id"] = item.item_id;
  json["conversation_id"] = item.conversation_id;
  json["kind"] = ItemKindName(item.kind);
  json["actor"] = {
      {"actor_id", item.actor.actor_id},
      {"kind", ActorKindName(item.actor.kind)},
      {"display_name", item.actor.display_name},
  };
  if (item.reply_to_item_id.has_value()) {
    json["reply_to_item_id"] = item.reply_to_item_id.value();
  }
  if (!item.mentions.empty()) {
    json["mentions"] = item.mentions;
  }
  if (item.turn_group_id.has_value()) {
    json["turn_group_id"] = item.turn_group_id.value();
  }
  json["payload"] = item.payload;
  return json;
}

ConversationItem FromJson(const nlohmann::json& json) {
  ConversationItem item;
  if (json.contains("item_id")) {
    item.item_id = json["item_id"].get<std::string>();
  }
  if (json.contains("conversation_id")) {
    item.conversation_id = json["conversation_id"].get<std::string>();
  }
  item.kind = ItemKindFromString(json.at("kind").get<std::string>());

  const auto& actor = json.at("actor");
  item.actor.actor_id = actor.value("actor_id", "");
  item.actor.kind = ActorKindFromString(actor.value("kind", "human"));
  item.actor.display_name = actor.value("display_name", "");

  if (json.contains("reply_to_item_id")) {
    item.reply_to_item_id = json["reply_to_item_id"].get<std::string>();
  }
  if (json.contains("mentions")) {
    item.mentions = json["mentions"].get<std::vector<std::string>>();
  }
  if (json.contains("turn_group_id")) {
    item.turn_group_id = json["turn_group_id"].get<std::string>();
  }
  item.payload = json.value("payload", nlohmann::json::object());
  return item;
}

std::string SerializeConversationItem(const ConversationItem& item) {
  return ToJson(item).dump();
}

std::optional<ConversationItem> TryParseConversationItem(
    const std::string& serialized) {
  if (serialized.empty()) { return std::nullopt; }
  try {
    return FromJson(nlohmann::json::parse(serialized));
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace shizuru::core::conversation
