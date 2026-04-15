#include "conversation/render.h"

#include <sstream>

namespace shizuru::core::conversation {
namespace {

std::string EscapeAttr(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += ch; break;
    }
  }
  return out;
}

std::string EscapeText(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      default: out += ch; break;
    }
  }
  return out;
}

void AppendAttr(std::ostringstream& oss,
                const char* name,
                const std::string& value) {
  if (value.empty()) { return; }
  oss << ' ' << name << "=\"" << EscapeAttr(value) << '"';
}

bool IsDefaultSingleUser(const ConversationItem& item) {
  return item.kind == ItemKind::kHumanMessage &&
         item.actor.kind == ActorKind::kHuman &&
         (item.actor.actor_id.empty() || item.actor.actor_id == "user") &&
         item.actor.display_name.empty() &&
         !item.reply_to_item_id.has_value() &&
         item.mentions.empty() &&
         !item.turn_group_id.has_value();
}

}  // namespace

ContextMessage RenderForLlm(const ConversationItem& item) {
  ContextMessage msg;

  switch (item.kind) {
    case ItemKind::kHumanMessage: {
      msg.role = "user";
      const std::string text = item.payload.value("text", "");
      if (IsDefaultSingleUser(item)) {
        msg.content = text;
        return msg;
      }

      std::ostringstream oss;
      oss << "<message";
      AppendAttr(oss, "actor_id", item.actor.actor_id);
      AppendAttr(oss, "actor_name", item.actor.display_name);
      AppendAttr(oss, "actor_kind", ActorKindName(item.actor.kind));
      if (item.reply_to_item_id.has_value()) {
        AppendAttr(oss, "reply_to", item.reply_to_item_id.value());
      }
      if (!item.mentions.empty()) {
        std::string joined;
        for (size_t i = 0; i < item.mentions.size(); ++i) {
          if (i > 0) { joined += ","; }
          joined += item.mentions[i];
        }
        AppendAttr(oss, "mentions", joined);
      }
      if (item.turn_group_id.has_value()) {
        AppendAttr(oss, "turn_group_id", item.turn_group_id.value());
      }
      oss << ">" << EscapeText(text) << "</message>";
      msg.content = oss.str();
      if (!item.actor.actor_id.empty()) {
        msg.name = item.actor.actor_id;  // Provider hint only.
      }
      return msg;
    }

    case ItemKind::kAssistantMessage:
      msg.role = "assistant";
      msg.content = item.payload.value("text", "");
      return msg;

    case ItemKind::kToolCall:
      msg.role = "assistant";
      if (item.payload.contains("tool_calls")) {
        msg.tool_calls_json = item.payload["tool_calls"].dump();
      } else {
        msg.tool_calls_json = "[]";
      }
      return msg;

    case ItemKind::kSystemEvent: {
      msg.role = "user";
      std::ostringstream oss;
      oss << "<event";
      AppendAttr(oss, "actor_id", item.actor.actor_id);
      AppendAttr(oss, "actor_name", item.actor.display_name);
      AppendAttr(oss, "actor_kind", ActorKindName(item.actor.kind));
      AppendAttr(oss, "event_type", item.payload.value("event_type", "event"));
      AppendAttr(oss, "source", item.payload.value("source", ""));
      oss << ">";
      if (item.payload.contains("data")) {
        oss << item.payload["data"].dump();
      } else {
        oss << "{}";
      }
      oss << "</event>";
      msg.content = oss.str();
      const std::string source = item.payload.value("source", "");
      if (!source.empty()) {
        msg.name = source;  // Provider hint only.
      } else if (!item.actor.actor_id.empty()) {
        msg.name = item.actor.actor_id;
      }
      return msg;
    }

    case ItemKind::kToolResult: {
      msg.role = "tool";
      msg.name = item.payload.value("tool_name", item.actor.display_name);
      msg.tool_call_id = item.payload.value("tool_call_id", "");
      if (item.payload.contains("content")) {
        const auto& content = item.payload["content"];
        msg.content = content.is_string() ? content.get<std::string>()
                                          : content.dump();
      } else {
        msg.content = "{}";
      }
      return msg;
    }
  }

  return msg;
}

}  // namespace shizuru::core::conversation
