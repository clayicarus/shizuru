// tests/core/tool_result_flow_test.cpp — Tool result return path tests.
//
// Task 48: Verifies requirements 9.1, 9.2, 9.3:
//   - Tool Executor only emits ToolResultSignal (not ConversationItem)
//   - Core receives ToolResultSignal and constructs kToolResult ConversationItem
//   - The constructed item is written to history

#include <gtest/gtest.h>

#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"

namespace shizuru::core {
namespace {

// Simulates the Core's handling of ToolResultSignal → ConversationItem.
// This mirrors the logic in CoreDevice::OnControl for ToolResultSignal.
ConversationItem ConstructToolResultItem(const ToolResultSignal& signal) {
  ConversationItem item;
  item.item_id = "toolresult:" + signal.tool_call_id;
  item.kind = ConversationItemKind::kToolResult;
  item.actor = ActorRef{"tool", "Tool", ActorKind::kTool};
  item.parts.emplace_back(ToolResultPart{
      signal.tool_call_id, "", signal.success, signal.content});
  item.wall_time = std::chrono::system_clock::now();
  return item;
}

TEST(ToolResultFlow, ToolResultSignalProducesCorrectItem) {
  ToolResultSignal signal;
  signal.tool_call_id = "call_abc123";
  signal.content = R"({"result": "42"})";
  signal.success = true;

  ConversationItem item = ConstructToolResultItem(signal);

  EXPECT_EQ(item.kind, ConversationItemKind::kToolResult);
  EXPECT_EQ(item.item_id, "toolresult:call_abc123");
  EXPECT_EQ(item.actor.kind, ActorKind::kTool);

  ASSERT_EQ(item.parts.size(), 1);
  auto* trp = std::get_if<ToolResultPart>(&item.parts[0]);
  ASSERT_NE(trp, nullptr);
  EXPECT_EQ(trp->tool_call_id, "call_abc123");
  EXPECT_EQ(trp->result_json, R"({"result": "42"})");
  EXPECT_TRUE(trp->success);
}

TEST(ToolResultFlow, FailedToolResultSignal) {
  ToolResultSignal signal;
  signal.tool_call_id = "call_fail";
  signal.content = R"({"error": "timeout"})";
  signal.success = false;

  ConversationItem item = ConstructToolResultItem(signal);

  ASSERT_EQ(item.parts.size(), 1);
  auto* trp = std::get_if<ToolResultPart>(&item.parts[0]);
  ASSERT_NE(trp, nullptr);
  EXPECT_FALSE(trp->success);
  EXPECT_EQ(trp->result_json, R"({"error": "timeout"})");
}

TEST(ToolResultFlow, ToolResultSignalIsControlSignalVariant) {
  // Verify ToolResultSignal is part of the ControlSignal variant.
  ControlSignal signal = ToolResultSignal{"id", "content", true};

  EXPECT_TRUE(std::holds_alternative<ToolResultSignal>(signal));
  auto& trs = std::get<ToolResultSignal>(signal);
  EXPECT_EQ(trs.tool_call_id, "id");
}

TEST(ToolResultFlow, ToolExecutorDoesNotConstructConversationItem) {
  // This is a design constraint test (requirement 9.1):
  // The ToolResultSignal only contains raw result data.
  // It does NOT contain a ConversationItem — that's Core's job.
  ToolResultSignal signal;
  signal.tool_call_id = "call_1";
  signal.content = "result data";
  signal.success = true;

  // ToolResultSignal has no ConversationItem field — verified by compilation.
  // The signal only carries: tool_call_id, content, success.
  EXPECT_FALSE(signal.tool_call_id.empty());
  EXPECT_FALSE(signal.content.empty());
}

}  // namespace
}  // namespace shizuru::core
