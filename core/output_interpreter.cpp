// core/output_interpreter.cpp — Core output interpretation implementation.

#include "core/output_interpreter.h"

#include <chrono>
#include <utility>

namespace shizuru::core {

OutputInterpreter::OutputInterpreter(OutputInterpreterCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void OutputInterpreter::Interpret(const ActionCandidate& candidate,
                                  const std::string& conversation_id) {
  switch (candidate.type) {
    case ActionType::kResponse: {
      // Produce an assistant message ConversationItem.
      ConversationItem item;
      item.item_id = "assistant:" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
      item.conversation_id = conversation_id;
      item.kind = ConversationItemKind::kAssistantMessage;
      item.actor = ActorRef{"assistant", "Assistant", ActorKind::kAssistant};
      item.parts.emplace_back(TextPart{candidate.response_text});
      item.wall_time = std::chrono::system_clock::now();

      if (callbacks_.on_item) {
        callbacks_.on_item(std::move(item));
      }

      // Signal turn complete.
      if (callbacks_.on_signal) {
        callbacks_.on_signal(TurnCompleteSignal{});
      }
      break;
    }

    case ActionType::kToolCall: {
      // Produce a tool call ConversationItem with ToolCallParts.
      ConversationItem item;
      item.item_id = "toolcall:" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
      item.conversation_id = conversation_id;
      item.kind = ConversationItemKind::kToolCall;
      item.actor = ActorRef{"assistant", "Assistant", ActorKind::kAssistant};
      item.wall_time = std::chrono::system_clock::now();

      for (const auto& tc : candidate.tool_calls) {
        item.parts.emplace_back(ToolCallPart{tc.id, tc.name, tc.arguments});

        // Emit a ToolCallStartSignal for each tool call.
        if (callbacks_.on_signal) {
          callbacks_.on_signal(
              ToolCallStartSignal{tc.id, tc.name, tc.arguments});
        }
      }

      if (callbacks_.on_item) {
        callbacks_.on_item(std::move(item));
      }
      break;
    }

    case ActionType::kContinue: {
      // No ConversationItem produced — just signal continuation.
      // The controller will handle re-invoking the LLM.
      break;
    }
  }
}

}  // namespace shizuru::core
