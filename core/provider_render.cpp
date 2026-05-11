// services/llm/openai/provider_render.cpp — Provider Render implementation.

#include "core/provider_render.h"

#include <string>

namespace shizuru::services {

using json = nlohmann::json;

nlohmann::json RenderItem(const core::ConversationItem& item,
                          bool multi_actor) {
  json msg;

  switch (item.kind) {
    case core::ConversationItemKind::kUserMessage: {
      msg["role"] = "user";

      // Build content — may be a string or array depending on parts.
      if (item.parts.size() == 1) {
        if (auto* tp = std::get_if<core::TextPart>(&item.parts[0])) {
          std::string text = tp->text;
          if (multi_actor && !item.actor.display_name.empty()) {
            text = "<message from=\"" + item.actor.display_name + "\">" +
                   text + "</message>";
          }
          msg["content"] = text;
        }
      } else {
        json content = json::array();
        for (const auto& part : item.parts) {
          if (auto* tp = std::get_if<core::TextPart>(&part)) {
            std::string text = tp->text;
            if (multi_actor && !item.actor.display_name.empty()) {
              text = "<message from=\"" + item.actor.display_name + "\">" +
                     text + "</message>";
            }
            content.push_back({{"type", "text"}, {"text", text}});
          } else if (auto* ip = std::get_if<core::ImagePart>(&part)) {
            content.push_back({
                {"type", "image_url"},
                {"image_url", {{"url", ip->url}}}});
          }
        }
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
      // Each ToolResultPart becomes a separate tool message.
      // For simplicity, render the first one here.
      // Multiple results should be rendered as multiple messages by the caller.
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

nlohmann::json RenderMessages(
    const std::vector<core::ConversationItem>& history,
    const core::InvokeBatch& batch,
    const std::string& system_instruction) {
  json messages = json::array();

  // System instruction first.
  if (!system_instruction.empty()) {
    messages.push_back({{"role", "system"}, {"content", system_instruction}});
  }

  // Detect multi-actor scenario (more than one unique actor_id in the batch).
  bool multi_actor = false;
  {
    std::string first_actor;
    for (const auto& item : history) {
      if (item.kind == core::ConversationItemKind::kUserMessage) {
        if (first_actor.empty()) {
          first_actor = item.actor.actor_id;
        } else if (item.actor.actor_id != first_actor) {
          multi_actor = true;
          break;
        }
      }
    }
    if (!multi_actor) {
      for (const auto& item : batch.items) {
        if (item.kind == core::ConversationItemKind::kUserMessage) {
          if (first_actor.empty()) {
            first_actor = item.actor.actor_id;
          } else if (item.actor.actor_id != first_actor) {
            multi_actor = true;
            break;
          }
        }
      }
    }
  }

  // Render history items.
  for (const auto& item : history) {
    // For kToolResult with multiple parts, render each as a separate message.
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
    } else {
      messages.push_back(RenderItem(item, multi_actor));
    }
  }

  // Render batch items.
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
    } else {
      messages.push_back(RenderItem(item, multi_actor));
    }
  }

  return messages;
}

}  // namespace shizuru::services
