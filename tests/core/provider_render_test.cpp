// tests/core/provider_render_test.cpp — Provider render tests.
//
// Tests for Tasks 44 and 45:
//   - Multiple parallel tool calls render correctly
//   - Multiple tool results render correctly

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/content_part.h"
#include "core/conversation_item.h"
#include "core/invoke_batch.h"
#include "services/llm/openai/provider_render.h"

namespace shizuru::services {
namespace {

using json = nlohmann::json;

// Task 44: Multiple parallel tool calls render as a single assistant message
// with a tool_calls array.
TEST(ProviderRender, ParallelToolCallsRenderCorrectly) {
  core::ConversationItem tool_call_item;
  tool_call_item.item_id = "tc1";
  tool_call_item.conversation_id = "conv1";
  tool_call_item.kind = core::ConversationItemKind::kToolCall;
  tool_call_item.actor = {"assistant", "Assistant", core::ActorKind::kAssistant};
  tool_call_item.parts.emplace_back(
      core::ToolCallPart{"call_1", "search", R"({"query":"hello"})"});
  tool_call_item.parts.emplace_back(
      core::ToolCallPart{"call_2", "fetch", R"({"url":"http://x.com"})"});

  json msg = RenderItem(tool_call_item, false);

  EXPECT_EQ(msg["role"], "assistant");
  EXPECT_TRUE(msg["content"].is_null());
  ASSERT_TRUE(msg.contains("tool_calls"));
  ASSERT_EQ(msg["tool_calls"].size(), 2);

  EXPECT_EQ(msg["tool_calls"][0]["id"], "call_1");
  EXPECT_EQ(msg["tool_calls"][0]["type"], "function");
  EXPECT_EQ(msg["tool_calls"][0]["function"]["name"], "search");
  EXPECT_EQ(msg["tool_calls"][0]["function"]["arguments"], R"({"query":"hello"})");

  EXPECT_EQ(msg["tool_calls"][1]["id"], "call_2");
  EXPECT_EQ(msg["tool_calls"][1]["function"]["name"], "fetch");
}

// Task 44: Parallel tool calls in full message rendering.
TEST(ProviderRender, ParallelToolCallsInFullRender) {
  std::vector<core::ConversationItem> history;

  // User message.
  core::ConversationItem user_msg;
  user_msg.item_id = "u1";
  user_msg.kind = core::ConversationItemKind::kUserMessage;
  user_msg.actor = {"user1", "User", core::ActorKind::kHuman};
  user_msg.parts.emplace_back(core::TextPart{"search for cats and dogs"});
  history.push_back(std::move(user_msg));

  // Tool call with 2 parallel calls.
  core::ConversationItem tc;
  tc.item_id = "tc1";
  tc.kind = core::ConversationItemKind::kToolCall;
  tc.actor = {"assistant", "Assistant", core::ActorKind::kAssistant};
  tc.parts.emplace_back(core::ToolCallPart{"c1", "search", R"({"q":"cats"})"});
  tc.parts.emplace_back(core::ToolCallPart{"c2", "search", R"({"q":"dogs"})"});
  history.push_back(std::move(tc));

  core::InvokeBatch batch;
  batch.conversation_id = "conv1";

  json messages = RenderMessages(history, batch, "You are helpful.");

  ASSERT_GE(messages.size(), 3);  // system + user + tool_call
  EXPECT_EQ(messages[0]["role"], "system");
  EXPECT_EQ(messages[1]["role"], "user");
  EXPECT_EQ(messages[1]["content"],
            "<message id=\"user1\" name=\"User\">search for cats and dogs</message>");
  EXPECT_EQ(messages[2]["role"], "assistant");
  EXPECT_EQ(messages[2]["tool_calls"].size(), 2);
}

// Task 45: Multiple tool results render as separate tool messages.
TEST(ProviderRender, MultipleToolResultsRenderAsSeparateMessages) {
  core::ConversationItem result_item;
  result_item.item_id = "tr1";
  result_item.kind = core::ConversationItemKind::kToolResult;
  result_item.actor = {"tool", "Tool", core::ActorKind::kTool};
  result_item.parts.emplace_back(
      core::ToolResultPart{"call_1", "search", true, R"({"results":["cat"]})"});
  result_item.parts.emplace_back(
      core::ToolResultPart{"call_2", "search", true, R"({"results":["dog"]})"});

  std::vector<core::ConversationItem> history = {result_item};
  core::InvokeBatch batch;
  batch.conversation_id = "conv1";

  json messages = RenderMessages(history, batch, "");

  // Multiple ToolResultParts should render as separate tool messages.
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(messages[0]["role"], "tool");
  EXPECT_EQ(messages[0]["tool_call_id"], "call_1");
  EXPECT_EQ(messages[0]["content"], R"({"results":["cat"]})");

  EXPECT_EQ(messages[1]["role"], "tool");
  EXPECT_EQ(messages[1]["tool_call_id"], "call_2");
  EXPECT_EQ(messages[1]["content"], R"({"results":["dog"]})");
}

// Task 45: Single tool result renders as one tool message.
TEST(ProviderRender, SingleToolResultRendersCorrectly) {
  core::ConversationItem result_item;
  result_item.item_id = "tr1";
  result_item.kind = core::ConversationItemKind::kToolResult;
  result_item.actor = {"tool", "Tool", core::ActorKind::kTool};
  result_item.parts.emplace_back(
      core::ToolResultPart{"call_1", "search", true, R"({"ok":true})"});

  json msg = RenderItem(result_item, false);

  EXPECT_EQ(msg["role"], "tool");
  EXPECT_EQ(msg["tool_call_id"], "call_1");
  EXPECT_EQ(msg["content"], R"({"ok":true})");
}

TEST(ProviderRender, UserImageOnlyMessageRendersContentArray) {
  core::ConversationItem user_msg;
  user_msg.item_id = "u1";
  user_msg.kind = core::ConversationItemKind::kUserMessage;
  user_msg.actor = {"user1", "User", core::ActorKind::kHuman};
  user_msg.parts.emplace_back(
      core::ImagePart{"https://example.com/cat.png"});

  json msg = RenderItem(user_msg, false);

  EXPECT_EQ(msg["role"], "user");
  ASSERT_TRUE(msg["content"].is_array());
  ASSERT_EQ(msg["content"].size(), 1);
  EXPECT_EQ(msg["content"][0]["type"], "image_url");
  EXPECT_EQ(msg["content"][0]["image_url"]["url"],
            "https://example.com/cat.png");
}

TEST(ProviderRender, UserMixedTextAndImageMessageRendersBothParts) {
  core::ConversationItem user_msg;
  user_msg.item_id = "u1";
  user_msg.kind = core::ConversationItemKind::kUserMessage;
  user_msg.actor = {"user1", "Alice", core::ActorKind::kHuman};
  user_msg.parts.emplace_back(core::TextPart{"look at this"});
  user_msg.parts.emplace_back(
      core::ImagePart{"https://example.com/cat.png"});

  json msg = RenderItem(user_msg, true);

  EXPECT_EQ(msg["role"], "user");
  ASSERT_TRUE(msg["content"].is_array());
  ASSERT_EQ(msg["content"].size(), 2);
  EXPECT_EQ(msg["content"][0]["type"], "text");
  EXPECT_EQ(msg["content"][0]["text"],
            "<message id=\"user1\" name=\"Alice\">look at this</message>");
  EXPECT_EQ(msg["content"][1]["type"], "image_url");
  EXPECT_EQ(msg["content"][1]["image_url"]["url"],
            "https://example.com/cat.png");
}

TEST(ProviderRender, HistoryImagesAreFilteredButBatchImagesRemain) {
  core::ConversationItem historical_image;
  historical_image.item_id = "history-image";
  historical_image.kind = core::ConversationItemKind::kUserMessage;
  historical_image.actor = {"user1", "Alice", core::ActorKind::kHuman};
  historical_image.parts.emplace_back(
      core::ImagePart{"https://example.com/old.png"});

  core::ConversationItem historical_text;
  historical_text.item_id = "history-text";
  historical_text.kind = core::ConversationItemKind::kUserMessage;
  historical_text.actor = {"user1", "Alice", core::ActorKind::kHuman};
  historical_text.parts.emplace_back(core::TextPart{"earlier text"});

  core::ConversationItem batch_image;
  batch_image.item_id = "batch-image";
  batch_image.kind = core::ConversationItemKind::kUserMessage;
  batch_image.actor = {"user1", "Alice", core::ActorKind::kHuman};
  batch_image.parts.emplace_back(
      core::ImagePart{"https://example.com/current.png"});

  core::InvokeBatch batch;
  batch.conversation_id = "conv1";
  batch.items.push_back(batch_image);

  json messages = RenderMessages(
      {historical_image, historical_text}, batch, "You are helpful.");

  ASSERT_EQ(messages.size(), 3);
  EXPECT_EQ(messages[0]["role"], "system");
  EXPECT_EQ(messages[1]["role"], "user");
  EXPECT_EQ(messages[1]["content"],
            "<message id=\"user1\" name=\"Alice\">earlier text</message>");
  EXPECT_EQ(messages[2]["role"], "user");
  ASSERT_TRUE(messages[2]["content"].is_array());
  ASSERT_EQ(messages[2]["content"].size(), 1);
  EXPECT_EQ(messages[2]["content"][0]["type"], "image_url");
  EXPECT_EQ(messages[2]["content"][0]["image_url"]["url"],
            "https://example.com/current.png");
}

TEST(ProviderRender, SingleActorUserMessageStillIncludesActorEnvelope) {
  core::ConversationItem user_msg;
  user_msg.item_id = "u1";
  user_msg.kind = core::ConversationItemKind::kUserMessage;
  user_msg.actor = {"user1", "icarus", core::ActorKind::kHuman};
  user_msg.parts.emplace_back(core::TextPart{"你知道我是谁吗"});

  json msg = RenderItem(user_msg, false);

  EXPECT_EQ(msg["role"], "user");
  EXPECT_EQ(msg["content"],
            "<message id=\"user1\" name=\"icarus\">你知道我是谁吗</message>");
}

}  // namespace
}  // namespace shizuru::services
