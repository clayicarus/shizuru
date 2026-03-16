// Unit tests for ContextStrategy
// Tests specific examples, edge cases, and integration points.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "context/types.h"
#include "controller/types.h"
#include "mock_memory_store.h"

namespace shizuru::core {
namespace {

// Helper to create a simple Observation.
Observation MakeObservation(const std::string& content,
                            ObservationType type = ObservationType::kUserMessage) {
  Observation obs;
  obs.type = type;
  obs.content = content;
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  return obs;
}

// Helper to create a simple MemoryEntry.
MemoryEntry MakeEntry(MemoryEntryType type, const std::string& role,
                      const std::string& content,
                      const std::string& tool_call_id = "") {
  MemoryEntry entry;
  entry.type = type;
  entry.role = role;
  entry.content = content;
  entry.tool_call_id = tool_call_id;
  entry.timestamp = std::chrono::steady_clock::now();
  entry.estimated_tokens = static_cast<int>(content.size()) / 4;
  return entry;
}

// ---------------------------------------------------------------------------
// Test: Context window assembly with known inputs
// Requirements: 5.1, 5.2, 7.2
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, AssemblyWithKnownInputs) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.default_system_instruction = "You are a helpful assistant.";

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "Be concise.");

  store.Append(sid, MakeEntry(MemoryEntryType::kUserMessage, "user", "Hi"));
  store.Append(sid, MakeEntry(MemoryEntryType::kAssistantMessage, "assistant",
                              "Hello!"));

  auto window = strategy.BuildContext(sid, MakeObservation("How are you?"));

  // Expected order: system, user-Hi, assistant-Hello!, user-HowAreYou
  ASSERT_EQ(window.messages.size(), 4u);
  EXPECT_EQ(window.messages[0].role, "system");
  EXPECT_EQ(window.messages[0].content, "Be concise.");
  EXPECT_EQ(window.messages[1].role, "user");
  EXPECT_EQ(window.messages[1].content, "Hi");
  EXPECT_EQ(window.messages[2].role, "assistant");
  EXPECT_EQ(window.messages[2].content, "Hello!");
  EXPECT_EQ(window.messages[3].role, "user");
  EXPECT_EQ(window.messages[3].content, "How are you?");
}

// ---------------------------------------------------------------------------
// Test: Token budget edge case — budget exactly equals content
// Requirements: 5.3, 5.4
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, TokenBudgetExactlyEqualsContent) {
  // System instruction: "sys" → 3 chars → 0 tokens (3/4 = 0)
  // Observation: "obs!" → 4 chars → 1 token
  // Memory entry: "memo" → 4 chars → 1 token
  // Total = 0 + 1 + 1 = 2 tokens
  ContextConfig config;
  config.max_context_tokens = 2;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "sys");

  store.Append(sid, MakeEntry(MemoryEntryType::kUserMessage, "user", "memo"));

  auto window = strategy.BuildContext(sid, MakeObservation("obs!"));

  // Budget is exactly 2, content fits exactly.
  EXPECT_LE(window.estimated_tokens, 2);
  // System and observation must always be present.
  ASSERT_GE(window.messages.size(), 2u);
  EXPECT_EQ(window.messages.front().role, "system");
  EXPECT_EQ(window.messages.back().content, "obs!");
}

// ---------------------------------------------------------------------------
// Test: Token budget forces truncation of older entries
// Requirements: 5.3, 5.4
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, TokenBudgetTruncatesOlderEntries) {
  // System: "a" → 0 tokens. Observation: "bbbb" → 1 token.
  // Fixed cost = 1. Budget = 2 → 1 token for memory.
  // Two memory entries of 1 token each → only the newest should fit.
  ContextConfig config;
  config.max_context_tokens = 2;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "a");

  store.Append(sid, MakeEntry(MemoryEntryType::kUserMessage, "user", "old1"));  // 1 token
  store.Append(sid, MakeEntry(MemoryEntryType::kAssistantMessage, "assistant", "new1"));  // 1 token

  auto window = strategy.BuildContext(sid, MakeObservation("bbbb"));

  EXPECT_LE(window.estimated_tokens, 2);
  // System + at most 1 memory entry + observation.
  ASSERT_GE(window.messages.size(), 2u);
  EXPECT_EQ(window.messages.front().role, "system");
  EXPECT_EQ(window.messages.back().content, "bbbb");

  // The newest memory entry should be kept over the oldest.
  bool has_new = false;
  for (size_t i = 1; i + 1 < window.messages.size(); ++i) {
    if (window.messages[i].content == "new1") has_new = true;
  }
  EXPECT_TRUE(has_new);
}

// ---------------------------------------------------------------------------
// Test: Tool call/result pairing
// Requirements: 5.5
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, ToolCallResultPairing) {
  ContextConfig config;
  config.max_context_tokens = 100000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "System");

  store.Append(sid, MakeEntry(MemoryEntryType::kUserMessage, "user", "Search for X"));
  store.Append(sid, MakeEntry(MemoryEntryType::kToolCall, "assistant",
                              "call_search", "tc_1"));
  store.Append(sid, MakeEntry(MemoryEntryType::kToolResult, "tool",
                              "result_data", "tc_1"));
  store.Append(sid, MakeEntry(MemoryEntryType::kAssistantMessage, "assistant",
                              "Here is what I found"));

  auto window = strategy.BuildContext(sid, MakeObservation("Thanks"));

  // Find the tool call and result in the window.
  int call_idx = -1, result_idx = -1;
  for (size_t i = 0; i < window.messages.size(); ++i) {
    if (window.messages[i].content == "call_search") call_idx = static_cast<int>(i);
    if (window.messages[i].content == "result_data") result_idx = static_cast<int>(i);
  }

  ASSERT_GE(call_idx, 0);
  ASSERT_GE(result_idx, 0);
  EXPECT_EQ(result_idx, call_idx + 1);
}

 TEST(ContextStrategyTest, ToolCallJsonConsumesBudget) {
  ContextConfig config;
  config.max_context_tokens = 2;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "sys!");

  MemoryEntry call;
  call.type = MemoryEntryType::kToolCall;
  call.role = "assistant";
  call.tool_call_id = "tc_1";
  call.tool_calls_json =
      R"([{"id":"call_1","type":"function","function":{"name":"search","arguments":{"q":"test"}}}])";
  call.timestamp = std::chrono::steady_clock::now();
  store.Append(sid, call);

  auto window = strategy.BuildContext(sid, MakeObservation("obs!"));

  EXPECT_EQ(window.messages.size(), 2u);
  EXPECT_EQ(window.messages[0].role, "system");
  EXPECT_EQ(window.messages[0].content, "sys!");
  EXPECT_EQ(window.messages[1].role, "user");
  EXPECT_EQ(window.messages[1].content, "obs!");
  EXPECT_LE(window.estimated_tokens, 2);
 }

// ---------------------------------------------------------------------------
// Test: Summarization trigger at exact threshold
// Requirements: 6.2
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, SummarizationAtExactThreshold) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.summarization_threshold = 5;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid);

  // Add exactly threshold entries — no summarization yet.
  for (int i = 0; i < 5; ++i) {
    strategy.RecordTurn(
        sid, MakeEntry(MemoryEntryType::kUserMessage, "user",
                       "msg" + std::to_string(i)));
  }
  EXPECT_EQ(store.GetAll(sid).size(), 5u);

  // Add one more to exceed threshold — summarization should trigger.
  strategy.RecordTurn(
      sid, MakeEntry(MemoryEntryType::kUserMessage, "user", "msg5"));

  auto all = store.GetAll(sid);
  EXPECT_LT(all.size(), 6u);

  // Verify a summary entry exists.
  bool has_summary = false;
  for (const auto& e : all) {
    if (e.type == MemoryEntryType::kSummary) {
      has_summary = true;
      break;
    }
  }
  EXPECT_TRUE(has_summary);
}

// ---------------------------------------------------------------------------
// Test: Default system instruction fallback
// Requirements: 7.1, 7.4
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, DefaultSystemInstructionFallback) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.default_system_instruction = "I am the default.";

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  // Init with empty string → should use default.
  strategy.InitSession(sid, "");

  auto window = strategy.BuildContext(sid, MakeObservation("Hello"));

  ASSERT_GE(window.messages.size(), 2u);
  EXPECT_EQ(window.messages[0].role, "system");
  EXPECT_EQ(window.messages[0].content, "I am the default.");
}

// ---------------------------------------------------------------------------
// Test: Default system instruction when no instruction provided at all
// Requirements: 7.4
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, DefaultSystemInstructionNoArg) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.default_system_instruction = "Default instruction.";

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid);  // No second argument.

  auto window = strategy.BuildContext(sid, MakeObservation("Hi"));

  ASSERT_GE(window.messages.size(), 2u);
  EXPECT_EQ(window.messages[0].content, "Default instruction.");
}

// ---------------------------------------------------------------------------
// Test: ReleaseSession clears everything
// Requirements: 6.4
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, ReleaseSessionClearsMemory) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.summarization_threshold = 1000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "System");

  store.Append(sid, MakeEntry(MemoryEntryType::kUserMessage, "user", "Hello"));
  store.Append(sid, MakeEntry(MemoryEntryType::kAssistantMessage, "assistant", "Hi"));

  EXPECT_EQ(store.GetAll(sid).size(), 2u);

  strategy.ReleaseSession(sid);

  EXPECT_TRUE(store.GetAll(sid).empty());
  EXPECT_TRUE(store.GetRecent(sid, 10).empty());
}

// ---------------------------------------------------------------------------
// Test: SetSystemInstruction mid-session
// Requirements: 7.3
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, SetSystemInstructionMidSession) {
  ContextConfig config;
  config.max_context_tokens = 100000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "Original instruction");

  auto obs = MakeObservation("test");

  auto w1 = strategy.BuildContext(sid, obs);
  EXPECT_EQ(w1.messages[0].content, "Original instruction");

  strategy.SetSystemInstruction(sid, "Updated instruction");

  auto w2 = strategy.BuildContext(sid, obs);
  EXPECT_EQ(w2.messages[0].content, "Updated instruction");
}

// ---------------------------------------------------------------------------
// Test: InjectContext with source tag
// Requirements: 6.5
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, InjectContextWithSourceTag) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.summarization_threshold = 1000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string sid = "s1";
  strategy.InitSession(sid, "System");

  MemoryEntry ext;
  ext.type = MemoryEntryType::kExternalContext;
  ext.role = "system";
  ext.content = "User profile: likes cats";
  ext.source_tag = "user_profile";
  ext.timestamp = std::chrono::steady_clock::now();

  strategy.InjectContext(sid, ext);

  auto all = store.GetAll(sid);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].content, "User profile: likes cats");
  EXPECT_EQ(all[0].source_tag, "user_profile");
  EXPECT_EQ(all[0].type, MemoryEntryType::kExternalContext);
}

// ---------------------------------------------------------------------------
// Test: Multiple sessions are isolated
// Requirements: 6.1, 6.4
// ---------------------------------------------------------------------------
TEST(ContextStrategyTest, MultipleSessionsIsolated) {
  ContextConfig config;
  config.max_context_tokens = 100000;
  config.summarization_threshold = 1000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  strategy.InitSession("s1", "Instruction A");
  strategy.InitSession("s2", "Instruction B");

  store.Append("s1", MakeEntry(MemoryEntryType::kUserMessage, "user", "Hello from s1"));
  store.Append("s2", MakeEntry(MemoryEntryType::kUserMessage, "user", "Hello from s2"));

  auto w1 = strategy.BuildContext("s1", MakeObservation("obs1"));
  auto w2 = strategy.BuildContext("s2", MakeObservation("obs2"));

  EXPECT_EQ(w1.messages[0].content, "Instruction A");
  EXPECT_EQ(w2.messages[0].content, "Instruction B");

  // Release s1 should not affect s2.
  strategy.ReleaseSession("s1");
  EXPECT_TRUE(store.GetAll("s1").empty());
  EXPECT_FALSE(store.GetAll("s2").empty());
}

}  // namespace
}  // namespace shizuru::core
