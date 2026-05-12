// services/llm/openai/provider_render.cpp — Provider Render implementation.

#include "llm/openai/provider_render.h"

#include <string>

namespace shizuru::services {

using json = nlohmann::json;

namespace {

json RenderUserContentPart(const core::ContentPart& part,
                           const core::ActorRef& actor,
                           bool include_images) {
  if (auto* tp = std::get_if<core::TextPart>(&part)) {
    std::string text = tp->text;
    if (!actor.actor_id.empty() || !actor.display_name.empty()) {
      text = "<message";
      if (!actor.actor_id.empty()) {
        text += " id=\"" + actor.actor_id + "\"";
      }
      if (!actor.display_name.empty()) {
        text += " name=\"" + actor.display_name + "\"";
      }
      text += ">" + tp->text + "</message>";
    }
    return {{"type", "text"}, {"text", std::move(text)}};
  }
  if (include_images) {
    if (auto* ip = std::get_if<core::ImagePart>(&part)) {
      return {
          {"type", "image_url"},
          {"image_url", {{"url", ip->url}}}};
    }
  }
  return nullptr;
}

json RenderItemImpl(const core::ConversationItem& item, bool include_images) {
  json msg;

  switch (item.kind) {
    case core::ConversationItemKind::kUserMessage: {
      msg["role"] = "user";
      json content = json::array();
      for (const auto& part : item.parts) {
        json rendered = RenderUserContentPart(part, item.actor, include_images);
        if (!rendered.is_null()) {
          content.push_back(std::move(rendered));
        }
      }

      if (content.empty()) {
        if (!include_images) {
          return nullptr;
        }
        msg["content"] = "[unsupported message]";
      } else if (content.size() == 1 && content[0].value("type", "") == "text") {
        msg["content"] = content[0]["text"];
      } else {
        msg["content"] = std::move(content);
      }
      break;
    }

    case core::ConversationItemKind::kAssistantMessage: {
      msg["role"] = "assistant";
      std::string text;
      for (const auto& part : item.parts) {
        if (auto* tp = std::get_if<core::TextPart>(&part)) {
          text += tp->text;
        }
      }
      msg["content"] = text;
      break;
    }

    case core::ConversationItemKind::kSystemEvent: {
      msg["role"] = "system";
      std::string text;
      for (const auto& part : item.parts) {
        if (auto* tp = std::get_if<core::TextPart>(&part)) {
          text += tp->text;
        }
      }
      msg["content"] = text;
      break;
    }

    case core::ConversationItemKind::kToolCall: {
      msg["role"] = "assistant";
      msg["content"] = nullptr;
      json tool_calls = json::array();
      for (const auto& part : item.parts) {
        if (auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
          tool_calls.push_back({
              {"id", tcp->tool_call_id},
              {"type", "function"},
              {"function", {
                  {"name", tcp->name},
                  {"arguments", tcp->arguments_json},
              }},
          });
        }
      }
      msg["tool_calls"] = std::move(tool_calls);
      break;
    }

    case core::ConversationItemKind::kToolResult: {
      for (const auto& part : item.parts) {
        if (auto* trp = std::get_if<core::ToolResultPart>(&part)) {
          msg["role"] = "tool";
          msg["tool_call_id"] = trp->tool_call_id;
          msg["content"] = trp->result_json;
          break;
        }
      }
      break;
    }
  }

  return msg;
}

}  // namespace

nlohmann::json RenderItem(const core::ConversationItem& item,
                          bool /*multi_actor*/) {
  return RenderItemImpl(item, true);
}

nlohmann::json RenderMessages(
    const std::vector<core::ConversationItem>& history,
    const core::InvokeBatch& batch,
    const std::string& system_instruction) {
  json messages = json::array();

  if (!system_instruction.empty()) {
    messages.push_back({{"role", "system"}, {"content", system_instruction}});
  }

  for (const auto& item : history) {
    if (item.kind == core::ConversationItemKind::kToolResult &&
        item.parts.size() > 1) {
      for (const auto& part : item.parts) {
        if (auto* trp = std::get_if<core::ToolResultPart>(&part)) {
          json msg;
          msg["role"] = "tool";
          msg["tool_call_id"] = trp->tool_call_id;
          msg["content"] = trp->result_json;
          messages.push_back(std::move(msg));
        }
      }
      continue;
    }
    json rendered = RenderItemImpl(item, false);
    if (!rendered.is_null()) {
      messages.push_back(std::move(rendered));
    }
  }

  for (const auto& item : batch.items) {
    if (item.kind == core::ConversationItemKind::kToolResult &&
        item.parts.size() > 1) {
      for (const auto& part : item.parts) {
        if (auto* trp = std::get_if<core::ToolResultPart>(&part)) {
          json msg;
          msg["role"] = "tool";
          msg["tool_call_id"] = trp->tool_call_id;
          msg["content"] = trp->result_json;
          messages.push_back(std::move(msg));
        }
      }
      continue;
    }
    messages.push_back(RenderItemImpl(item, true));
  }

  return messages;
}

}  // namespace shizuru::services
