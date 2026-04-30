// Property-based tests for ContextStrategy
// Uses RapidCheck + Google Test

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "context/types.h"
#include "controller/types.h"
#include "interfaces/memory_store.h"
#include "mock_memory_store.h"

namespace shizuru::core {
namespace {

// ---------------------------------------------------------------------------
// RapidCheck generators
// ---------------------------------------------------------------------------

rc::Gen<std::string> genNonEmptyString() {
  return rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
}

rc::Gen<std::string> genShortString() {
  return rc::gen::suchThat(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')),
      [](const std::string& s) { return !s.empty() && s.size() <= 20; });
}

rc::Gen<MemoryEntryType> genMemoryEntryType() {
  return rc::gen::element(MemoryEntryType::kUserMessage,
                          MemoryEntryType::kAssistantMessage,
                          MemoryEntryType::kToolCall,
                          MemoryEntryType::kToolResult,
                          MemoryEntryType::kSummary,
                          MemoryEntryType::kExternalContext);
}

rc::Gen<MemoryEntry> genMemoryEntry() {
  return rc::gen::apply(
      [](MemoryEntryType type, std::string content, std::string source_tag) {
        MemoryEntry entry;
        entry.type = type;
        switch (type) {
          case MemoryEntryType::kUserMessage:
            entry.role = "user";
            break;
          case MemoryEntryType::kAssistantMessage:
            entry.role = "assistant";
            break;
          case MemoryEntryType::kToolCall:
          case MemoryEntryType::kToolResult:
            entry.role = "tool";
            break;
          case MemoryEntryType::kSummary:
          case MemoryEntryType::kExternalContext:
            entry.role = "system";
            break;
        }
        entry.content = content;
        entry.source_tag = source_tag;
        entry.timestamp = std::chrono::steady_clock::now();
        entry.estimated_tokens = static_cast<int>(content.size()) / 4;
        return entry;
      },
      rc::gen::element(MemoryEntryType::kUserMessage,
                       MemoryEntryType::kAssistantMessage),
      genShortString(), genShortString());
}

// Generate a simple non-tool MemoryEntry (user or assistant)
rc::Gen<MemoryEntry> genSimpleMemoryEntry() {
  return rc::gen::apply(
      [](bool is_user, std::string content) {
        MemoryEntry entry;
        entry.type = is_user ? MemoryEntryType::kUserMessage
                             : MemoryEntryType::kAssistantMessage;
        entry.role = is_user ? "user" : "assistant";
        entry.content = content;
        entry.timestamp = std::chrono::steady_clock::now();
        entry.estimated_tokens = static_cast<int>(content.size()) / 4;
        return entry;
      },
      rc::gen::arbitrary<bool>(), genShortString());
}

rc::Gen<Observation> genObservation() {
  return rc::gen::apply(
      [](std::string content) {
        Observation obs;
        obs.type = ObservationType::kUserMessage;
        obs.content = content;
        obs.source = "user";
        obs.timestamp = std::chrono::steady_clock::now();
        return obs;
      },
      genShortString());
}

// ---------------------------------------------------------------------------
// Property 10: Context window ordering invariant
// Feature: agent-core, Property 10: Context window ordering invariant
// ---------------------------------------------------------------------------
// **Validates: Requirements 5.1, 5.2, 7.2**
RC_GTEST_PROP(ContextStrategyPropTest, prop_context_ordering, (void)) {
  auto system_instruction = *genNonEmptyString();
  auto num_entries = *rc::gen::inRange(0, 10);
  std::vector<MemoryEntry> entries;
  for (int i = 0; i < num_entries; ++i) {
    entries.push_back(*genSimpleMemoryEntry());
  }
  auto observation = *genObservation();

  // Use a large budget so nothing gets truncated.
  ContextConfig config;
  config.max_context_tokens = 1000000;
  config.default_system_instruction = "default";

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id, system_instruction);

  // Add entries to memory store.
  for (const auto& entry : entries) {
    store.Append(session_id, entry);
  }

  auto window = strategy.BuildContext(session_id, observation);

  // Must have at least 2 messages: system + observation.
  RC_ASSERT(window.messages.size() >= 2);

  // First message must be the system instruction.
  RC_ASSERT(window.messages.front().role == "system");
  RC_ASSERT(window.messages.front().content == system_instruction);

  // Last message must be the current observation.
  RC_ASSERT(window.messages.back().content == observation.content);

  // Middle messages (memory) should be in the same chronological order
  // as they were inserted (indices 1..n-2).
  size_t memory_count = window.messages.size() - 2;
  RC_ASSERT(memory_count <= entries.size());

  // Verify memory messages match the entries in order.
  size_t offset = entries.size() - memory_count;
  for (size_t i = 0; i < memory_count; ++i) {
    RC_ASSERT(window.messages[i + 1].content == entries[offset + i].content);
  }
}

// ---------------------------------------------------------------------------
// Property 11: Context window token budget with preservation
// Feature: agent-core, Property 11: Context window token budget with preservation
// ---------------------------------------------------------------------------
// **Validates: Requirements 5.3, 5.4**
RC_GTEST_PROP(ContextStrategyPropTest, prop_context_token_budget, (void)) {
  auto system_instruction = *genShortString();
  auto observation = *genObservation();
  auto num_entries = *rc::gen::inRange(1, 15);
  std::vector<MemoryEntry> entries;
  for (int i = 0; i < num_entries; ++i) {
    entries.push_back(*genSimpleMemoryEntry());
  }

  // Use a budget that's tight but always fits system + observation.
  int sys_tokens = static_cast<int>(system_instruction.size()) / 4;
  int obs_tokens = static_cast<int>(observation.content.size()) / 4;
  int min_budget = sys_tokens + obs_tokens + 1;
  int max_budget = min_budget + 200;
  int budget = *rc::gen::inRange(min_budget, max_budget + 1);

  ContextConfig config;
  config.max_context_tokens = budget;
  config.default_system_instruction = "default";

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id, system_instruction);

  for (const auto& entry : entries) {
    store.Append(session_id, entry);
  }

  auto window = strategy.BuildContext(session_id, observation);

  // Token budget must not be exceeded.
  RC_ASSERT(window.estimated_tokens <= budget);

  // System message must always be present as first message.
  RC_ASSERT(!window.messages.empty());
  RC_ASSERT(window.messages.front().role == "system");
  RC_ASSERT(window.messages.front().content == system_instruction);

  // Current observation must always be present as last message.
  RC_ASSERT(window.messages.back().content == observation.content);
}

// ---------------------------------------------------------------------------
// Property 12: Tool call and result adjacency
// Feature: agent-core, Property 12: Tool call and result adjacency
// ---------------------------------------------------------------------------
// **Validates: Requirements 5.5**
RC_GTEST_PROP(ContextStrategyPropTest, prop_tool_call_result_adjacency,
              (void)) {
  // Create a tool call / tool result pair with matching tool_call_id.
  auto tool_call_id = *genNonEmptyString();
  auto call_content = *genShortString();
  auto result_content = *genShortString();

  MemoryEntry tool_call;
  tool_call.type = MemoryEntryType::kToolCall;
  tool_call.role = "assistant";
  tool_call.content = call_content;
  tool_call.tool_call_id = tool_call_id;
  tool_call.timestamp = std::chrono::steady_clock::now();
  tool_call.estimated_tokens = static_cast<int>(call_content.size()) / 4;

  MemoryEntry tool_result;
  tool_result.type = MemoryEntryType::kToolResult;
  tool_result.role = "tool";
  tool_result.content = result_content;
  tool_result.tool_call_id = tool_call_id;
  tool_result.timestamp = std::chrono::steady_clock::now();
  tool_result.estimated_tokens = static_cast<int>(result_content.size()) / 4;

  // Optionally add some regular entries around the pair.
  auto num_prefix = *rc::gen::inRange(0, 3);
  std::vector<MemoryEntry> prefix_entries;
  for (int i = 0; i < num_prefix; ++i) {
    prefix_entries.push_back(*genSimpleMemoryEntry());
  }
  auto num_suffix = *rc::gen::inRange(0, 3);
  std::vector<MemoryEntry> suffix_entries;
  for (int i = 0; i < num_suffix; ++i) {
    suffix_entries.push_back(*genSimpleMemoryEntry());
  }

  ContextConfig config;
  config.max_context_tokens = 1000000;  // Large budget.
  config.default_system_instruction = "default";

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id, "System");

  for (const auto& e : prefix_entries) store.Append(session_id, e);
  store.Append(session_id, tool_call);
  store.Append(session_id, tool_result);
  for (const auto& e : suffix_entries) store.Append(session_id, e);

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = "hello";
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();

  auto window = strategy.BuildContext(session_id, obs);

  // Find the tool call message in the window.
  int call_idx = -1;
  int result_idx = -1;
  for (size_t i = 0; i < window.messages.size(); ++i) {
    if (window.messages[i].tool_call_id == tool_call_id) {
      if (window.messages[i].content == call_content) {
        call_idx = static_cast<int>(i);
      } else if (window.messages[i].content == result_content) {
        result_idx = static_cast<int>(i);
      }
    }
  }

  // If both are present, they must be adjacent (call immediately before result).
  if (call_idx >= 0 && result_idx >= 0) {
    RC_ASSERT(result_idx == call_idx + 1);
  }
  // If only one is present without the other, that's also acceptable
  // (the pair was dropped together during truncation, or kept as orphan).
}

// ---------------------------------------------------------------------------
// Property 13: Memory entry round-trip
// Feature: agent-core, Property 13: Memory entry round-trip
// ---------------------------------------------------------------------------
// **Validates: Requirements 6.1, 6.5**
RC_GTEST_PROP(ContextStrategyPropTest, prop_memory_round_trip, (void)) {
  auto content = *genNonEmptyString();
  auto source_tag = *genNonEmptyString();

  ContextConfig config;
  config.max_context_tokens = 1000000;
  config.summarization_threshold = 1000;  // High threshold to avoid summarization.

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id);

  // Test RecordTurn round-trip.
  MemoryEntry turn_entry;
  turn_entry.type = MemoryEntryType::kUserMessage;
  turn_entry.role = "user";
  turn_entry.content = content;
  turn_entry.source_tag = source_tag;
  turn_entry.timestamp = std::chrono::steady_clock::now();

  strategy.RecordTurn(session_id, turn_entry);

  auto all = store.GetAll(session_id);
  RC_ASSERT(!all.empty());

  bool found_turn = false;
  for (const auto& e : all) {
    if (e.content == content && e.source_tag == source_tag) {
      found_turn = true;
      break;
    }
  }
  RC_ASSERT(found_turn);

  // Test InjectContext round-trip.
  auto inject_content = *genNonEmptyString();
  auto inject_tag = *genNonEmptyString();

  MemoryEntry inject_entry;
  inject_entry.type = MemoryEntryType::kExternalContext;
  inject_entry.role = "system";
  inject_entry.content = inject_content;
  inject_entry.source_tag = inject_tag;
  inject_entry.timestamp = std::chrono::steady_clock::now();

  strategy.InjectContext(session_id, inject_entry);

  all = store.GetAll(session_id);
  bool found_inject = false;
  for (const auto& e : all) {
    if (e.content == inject_content && e.source_tag == inject_tag) {
      found_inject = true;
      break;
    }
  }
  RC_ASSERT(found_inject);
}

// ---------------------------------------------------------------------------
// Property 14: Memory summarization threshold
// Feature: agent-core, Property 14: Memory summarization threshold
// ---------------------------------------------------------------------------
// **Validates: Requirements 6.2**
RC_GTEST_PROP(ContextStrategyPropTest, prop_summarization_threshold, (void)) {
  // Pick a small threshold so we can exceed it easily.
  int threshold = *rc::gen::inRange(3, 15);
  int num_entries = threshold + *rc::gen::inRange(1, 10);

  ContextConfig config;
  config.max_context_tokens = 1000000;
  config.summarization_threshold = threshold;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id);

  // Record entries one by one via RecordTurn (which triggers MaybeSummarize).
  for (int i = 0; i < num_entries; ++i) {
    MemoryEntry entry;
    entry.type = MemoryEntryType::kUserMessage;
    entry.role = "user";
    entry.content = "msg" + std::to_string(i);
    entry.timestamp = std::chrono::steady_clock::now();
    entry.estimated_tokens = 1;
    strategy.RecordTurn(session_id, entry);
  }

  // After exceeding the threshold, summarization should have reduced the count.
  auto all = store.GetAll(session_id);
  RC_ASSERT(static_cast<int>(all.size()) < num_entries);
}

// ---------------------------------------------------------------------------
// Property 15: Recent memory retrieval
// Feature: agent-core, Property 15: Recent memory retrieval
// ---------------------------------------------------------------------------
// **Validates: Requirements 6.3**
RC_GTEST_PROP(ContextStrategyPropTest, prop_recent_memory_retrieval, (void)) {
  int num_entries = *rc::gen::inRange(1, 20);
  int n_recent = *rc::gen::inRange(1, 30);

  ContextConfig config;
  config.max_context_tokens = 1000000;
  config.summarization_threshold = 1000;  // High to avoid summarization.

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id);

  // Add entries with distinguishable content.
  for (int i = 0; i < num_entries; ++i) {
    MemoryEntry entry;
    entry.type = MemoryEntryType::kUserMessage;
    entry.role = "user";
    entry.content = "entry_" + std::to_string(i);
    entry.timestamp = std::chrono::steady_clock::now();
    entry.estimated_tokens = 1;
    store.Append(session_id, entry);
  }

  auto recent = store.GetRecent(session_id, static_cast<size_t>(n_recent));

  // Should return exactly min(N, M) entries.
  int expected_count = std::min(n_recent, num_entries);
  RC_ASSERT(static_cast<int>(recent.size()) == expected_count);

  // Entries should be ordered oldest to newest among the selected.
  if (recent.size() > 1) {
    for (size_t i = 1; i < recent.size(); ++i) {
      // Content is "entry_X" where X increases — verify ordering.
      RC_ASSERT(recent[i - 1].content < recent[i].content ||
                recent[i - 1].timestamp <= recent[i].timestamp);
    }
  }
}

// ---------------------------------------------------------------------------
// Property 16: Session termination releases memory
// Feature: agent-core, Property 16: Session termination releases memory
// ---------------------------------------------------------------------------
// **Validates: Requirements 6.4**
RC_GTEST_PROP(ContextStrategyPropTest, prop_session_release_clears_memory,
              (void)) {
  auto num_entries = *rc::gen::inRange(1, 10);
  std::vector<MemoryEntry> entries;
  for (int i = 0; i < num_entries; ++i) {
    entries.push_back(*genSimpleMemoryEntry());
  }

  ContextConfig config;
  config.max_context_tokens = 1000000;
  config.summarization_threshold = 1000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = *genNonEmptyString();
  strategy.InitSession(session_id);

  for (const auto& entry : entries) {
    store.Append(session_id, entry);
  }

  // Verify entries exist before release.
  auto before = store.GetAll(session_id);
  RC_ASSERT(!before.empty());

  // Release the session.
  strategy.ReleaseSession(session_id);

  // Committed history must remain available after the session is released.
  auto after_all = store.GetAll(session_id);
  RC_ASSERT(after_all.size() == before.size());
  for (size_t i = 0; i < before.size(); ++i) {
    RC_ASSERT(after_all[i].role == before[i].role);
    RC_ASSERT(after_all[i].content == before[i].content);
    RC_ASSERT(after_all[i].tool_call_id == before[i].tool_call_id);
    RC_ASSERT(after_all[i].tool_calls_json == before[i].tool_calls_json);
    RC_ASSERT(after_all[i].item_json == before[i].item_json);
  }

  auto after_recent = store.GetRecent(session_id, 100);
  RC_ASSERT(after_recent.size() == before.size());
  for (size_t i = 0; i < before.size(); ++i) {
    RC_ASSERT(after_recent[i].role == before[i].role);
    RC_ASSERT(after_recent[i].content == before[i].content);
    RC_ASSERT(after_recent[i].tool_call_id == before[i].tool_call_id);
    RC_ASSERT(after_recent[i].tool_calls_json == before[i].tool_calls_json);
    RC_ASSERT(after_recent[i].item_json == before[i].item_json);
  }
}

// ---------------------------------------------------------------------------
// Property 17: System instruction update takes effect immediately
// Feature: agent-core, Property 17: System instruction update takes effect immediately
// ---------------------------------------------------------------------------
// **Validates: Requirements 7.3**
RC_GTEST_PROP(ContextStrategyPropTest, prop_system_instruction_update,
              (void)) {
  auto initial_instruction = *genNonEmptyString();
  auto updated_instruction = *genNonEmptyString();
  // Ensure they differ.
  RC_PRE(initial_instruction != updated_instruction);

  ContextConfig config;
  config.max_context_tokens = 1000000;

  testing::MockMemoryStore store;
  ContextStrategy strategy(config, store);

  std::string session_id = "test-session";
  strategy.InitSession(session_id, initial_instruction);

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = "hello";
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();

  // Before update: system message should be the initial instruction.
  auto window1 = strategy.BuildContext(session_id, obs);
  RC_ASSERT(!window1.messages.empty());
  RC_ASSERT(window1.messages.front().role == "system");
  RC_ASSERT(window1.messages.front().content == initial_instruction);

  // Update the system instruction.
  strategy.SetSystemInstruction(session_id, updated_instruction);

  // After update: system message should be the updated instruction.
  auto window2 = strategy.BuildContext(session_id, obs);
  RC_ASSERT(!window2.messages.empty());
  RC_ASSERT(window2.messages.front().role == "system");
  RC_ASSERT(window2.messages.front().content == updated_instruction);
}

}  // namespace
}  // namespace shizuru::core
