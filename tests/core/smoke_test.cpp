// tests/core/smoke_test.cpp — Phase 0 smoke tests for the unified pipeline.
//
// These 5 tests verify the main capability surfaces are functional:
// 1. Text conversation round-trip
// 2. Tool call round-trip
// 3. Voice agent (ASR final → Core)
// 4. History replay
// 5. Multimodal (text + image → provider render)

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/batching.h"
#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "core/history.h"
#include "core/invoke_batch.h"
#include "core/output_interpreter.h"
#include "services/llm/openai/provider_render.h"
#include "services/memory/in_memory_history.h"

namespace shizuru {
namespace {

using json = nlohmann::json;

// ==========================================================================
// Smoke Test 1: Text Conversation Round-Trip
// User input → ConversationItem → history → provider render → LLM messages
// ==========================================================================
TEST(SmokeTest, TextConversationRoundTrip) {
  services::InMemoryHistory history;
  const std::string session = "test-session";

  // 1. User sends a message.
  core::ConversationItem user_msg;
  user_msg.item_id = "msg:1";
  user_msg.conversation_id = session;
  user_msg.kind = core::ConversationItemKind::kUserMessage;
  user_msg.actor = core::ActorRef{"user1", "Alice", core::ActorKind::kHuman};
  user_msg.parts.emplace_back(core::TextPart{"Hello, how are you?"});
  user_msg.wall_time = std::chrono::system_clock::now();

  history.Append(session, user_msg);

  // 2. Simulate assistant response via OutputInterpreter.
  core::ActionCandidate response_ac;
  response_ac.type = core::ActionType::kResponse;
  response_ac.response_text = "I'm doing well, thanks!";

  std::vector<core::ConversationItem> produced_items;
  std::vector<core::ControlSignal> produced_signals;

  core::OutputInterpreter interpreter({
      [&](core::ConversationItem item) { produced_items.push_back(std::move(item)); },
      [&](core::ControlSignal sig) { produced_signals.push_back(std::move(sig)); },
  });
  interpreter.Interpret(response_ac, session);

  // 3. Verify assistant item was produced.
  ASSERT_EQ(produced_items.size(), 1);
  EXPECT_EQ(produced_items[0].kind, core::ConversationItemKind::kAssistantMessage);
  auto* tp = std::get_if<core::TextPart>(&produced_items[0].parts[0]);
  ASSERT_NE(tp, nullptr);
  EXPECT_EQ(tp->text, "I'm doing well, thanks!");

  // 4. Verify TurnComplete signal.
  ASSERT_EQ(produced_signals.size(), 1);
  EXPECT_TRUE(std::holds_alternative<core::TurnCompleteSignal>(produced_signals[0]));

  // 5. Write assistant response to history.
  history.Append(session, produced_items[0]);

  // 6. Verify history has both items.
  auto recent = history.GetRecent(session, 10);
  ASSERT_EQ(recent.size(), 2);
  EXPECT_EQ(recent[0].kind, core::ConversationItemKind::kUserMessage);
  EXPECT_EQ(recent[1].kind, core::ConversationItemKind::kAssistantMessage);
}

// ==========================================================================
// Smoke Test 2: Tool Call Round-Trip
// user → tool call → tool result signal → kToolResult item → continuation
// ==========================================================================
TEST(SmokeTest, ToolCallRoundTrip) {
  services::InMemoryHistory history;
  const std::string session = "test-session";

  // 1. User asks something that triggers a tool call.
  core::ConversationItem user_msg;
  user_msg.item_id = "msg:1";
  user_msg.conversation_id = session;
  user_msg.kind = core::ConversationItemKind::kUserMessage;
  user_msg.actor = core::ActorRef{"user1", "User", core::ActorKind::kHuman};
  user_msg.parts.emplace_back(core::TextPart{"What time is it?"});
  history.Append(session, user_msg);

  // 2. LLM returns a tool call.
  core::ActionCandidate tool_ac;
  tool_ac.type = core::ActionType::kToolCall;
  tool_ac.tool_calls = {{"call_1", "get_current_time", "{}", "builtin"}};

  std::vector<core::ConversationItem> items;
  std::vector<core::ControlSignal> signals;
  core::OutputInterpreter interpreter({
      [&](core::ConversationItem item) { items.push_back(std::move(item)); },
      [&](core::ControlSignal sig) { signals.push_back(std::move(sig)); },
  });
  interpreter.Interpret(tool_ac, session);

  // 3. Verify tool call item.
  ASSERT_EQ(items.size(), 1);
  EXPECT_EQ(items[0].kind, core::ConversationItemKind::kToolCall);
  auto* tcp = std::get_if<core::ToolCallPart>(&items[0].parts[0]);
  ASSERT_NE(tcp, nullptr);
  EXPECT_EQ(tcp->name, "get_current_time");
  EXPECT_EQ(tcp->tool_call_id, "call_1");

  // 4. Verify ToolCallStartSignal.
  ASSERT_EQ(signals.size(), 1);
  EXPECT_TRUE(std::holds_alternative<core::ToolCallStartSignal>(signals[0]));

  // 5. Simulate tool executor returning ToolResultSignal.
  core::ToolResultSignal result_signal;
  result_signal.tool_call_id = "call_1";
  result_signal.content = R"({"time":"2024-01-01 12:00:00"})";
  result_signal.success = true;

  // 6. Core constructs kToolResult ConversationItem from signal.
  core::ConversationItem tool_result_item;
  tool_result_item.item_id = "toolresult:call_1";
  tool_result_item.conversation_id = session;
  tool_result_item.kind = core::ConversationItemKind::kToolResult;
  tool_result_item.actor = core::ActorRef{"tool", "Tool", core::ActorKind::kTool};
  tool_result_item.parts.emplace_back(core::ToolResultPart{
      result_signal.tool_call_id, "get_current_time",
      result_signal.success, result_signal.content});
  tool_result_item.wall_time = std::chrono::system_clock::now();

  // 7. Write to history.
  history.Append(session, items[0]);  // tool call
  history.Append(session, tool_result_item);  // tool result

  // 8. Verify history.
  auto recent = history.GetRecent(session, 10);
  ASSERT_EQ(recent.size(), 3);  // user + tool_call + tool_result
  EXPECT_EQ(recent[1].kind, core::ConversationItemKind::kToolCall);
  EXPECT_EQ(recent[2].kind, core::ConversationItemKind::kToolResult);
}

// ==========================================================================
// Smoke Test 3: Voice Agent (ASR final → Core path)
// AudioFrame → ASR → final ConversationItem → Batcher → InvokeBatch
// ==========================================================================
TEST(SmokeTest, VoiceAgentAsrFinalToCore) {
  core::Batcher batcher;

  // Simulate ASR producing a final ConversationItem (only finals reach Core).
  core::ConversationItem asr_item;
  asr_item.item_id = "asr:12345";
  asr_item.conversation_id = "voice-session";
  asr_item.kind = core::ConversationItemKind::kUserMessage;
  asr_item.actor = core::ActorRef{"local-user", "User", core::ActorKind::kHuman};
  asr_item.parts.emplace_back(core::TextPart{"Hello world"});
  asr_item.wall_time = std::chrono::system_clock::now();

  // Enqueue into batcher.
  batcher.Enqueue(std::move(asr_item));
  EXPECT_TRUE(batcher.HasPending());

  // Flush to create InvokeBatch.
  core::InvokeBatch batch = batcher.Flush(core::TriggerReason::kUserFlush);

  ASSERT_EQ(batch.items.size(), 1);
  EXPECT_EQ(batch.items[0].kind, core::ConversationItemKind::kUserMessage);
  EXPECT_EQ(batch.conversation_id, "voice-session");
  EXPECT_EQ(batch.reason, core::TriggerReason::kUserFlush);

  // Verify the text content.
  auto* tp = std::get_if<core::TextPart>(&batch.items[0].parts[0]);
  ASSERT_NE(tp, nullptr);
  EXPECT_EQ(tp->text, "Hello world");
}

// ==========================================================================
// Smoke Test 4: History Replay
// Write items → GetRecent → verify order and content preserved
// ==========================================================================
TEST(SmokeTest, HistoryReplay) {
  services::InMemoryHistory history;
  const std::string session = "replay-session";

  // Write a conversation sequence.
  for (int i = 0; i < 5; ++i) {
    core::ConversationItem item;
    item.item_id = "item:" + std::to_string(i);
    item.conversation_id = session;
    item.kind = (i % 2 == 0) ? core::ConversationItemKind::kUserMessage
                              : core::ConversationItemKind::kAssistantMessage;
    item.actor = (i % 2 == 0)
        ? core::ActorRef{"user", "User", core::ActorKind::kHuman}
        : core::ActorRef{"assistant", "Assistant", core::ActorKind::kAssistant};
    item.parts.emplace_back(core::TextPart{"Message " + std::to_string(i)});
    item.wall_time = std::chrono::system_clock::now();
    history.Append(session, std::move(item));
  }

  // Replay: get recent items.
  auto replayed = history.GetRecent(session, 10);
  ASSERT_EQ(replayed.size(), 5);

  // Verify order is preserved (oldest first).
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(replayed[i].item_id, "item:" + std::to_string(i));
    auto* tp = std::get_if<core::TextPart>(&replayed[i].parts[0]);
    ASSERT_NE(tp, nullptr);
    EXPECT_EQ(tp->text, "Message " + std::to_string(i));
  }

  // Verify GetWindow with token budget.
  auto windowed = history.GetWindow(session, 20);  // ~20 tokens budget
  EXPECT_LE(windowed.size(), 5);
  EXPECT_GE(windowed.size(), 1);
}

// ==========================================================================
// Smoke Test 5: Multimodal (text + image → provider render)
// ==========================================================================
TEST(SmokeTest, MultimodalProviderRender) {
  // Build a user message with text + image.
  core::ConversationItem multimodal_msg;
  multimodal_msg.item_id = "msg:multi";
  multimodal_msg.conversation_id = "session";
  multimodal_msg.kind = core::ConversationItemKind::kUserMessage;
  multimodal_msg.actor = core::ActorRef{"user1", "User", core::ActorKind::kHuman};
  multimodal_msg.parts.emplace_back(core::TextPart{"What's in this image?"});
  multimodal_msg.parts.emplace_back(core::ImagePart{"https://example.com/cat.jpg"});
  multimodal_msg.wall_time = std::chrono::system_clock::now();

  // Render via provider_render. History images are filtered; current batch
  // images remain available to the provider.
  std::vector<core::ConversationItem> history;
  core::InvokeBatch batch;
  batch.conversation_id = "session";
  batch.items.push_back(multimodal_msg);

  json messages = services::RenderMessages(history, batch, "You are helpful.");

  // Verify structure.
  ASSERT_GE(messages.size(), 2);  // system + user

  // System message.
  EXPECT_EQ(messages[0]["role"], "system");

  // User message with content array (text + image_url).
  EXPECT_EQ(messages[1]["role"], "user");
  ASSERT_TRUE(messages[1]["content"].is_array());
  ASSERT_EQ(messages[1]["content"].size(), 2);

  // First part: text.
  EXPECT_EQ(messages[1]["content"][0]["type"], "text");
  EXPECT_EQ(messages[1]["content"][0]["text"],
            "<message id=\"user1\" name=\"User\">What's in this image?</message>");

  // Second part: image_url.
  EXPECT_EQ(messages[1]["content"][1]["type"], "image_url");
  EXPECT_EQ(messages[1]["content"][1]["image_url"]["url"],
            "https://example.com/cat.jpg");
}

}  // namespace
}  // namespace shizuru
