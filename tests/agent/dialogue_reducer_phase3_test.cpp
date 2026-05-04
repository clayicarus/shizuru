// Unit tests for DefaultDialogueReducer — Phase 3: workspace integration.
//
// Validates: Requirements 14.1, 14.2, 14.3

#include <gtest/gtest.h>

#include <chrono>

#include "controller/config.h"
#include "controller/types.h"
#include "conversation/item.h"
#include "dialogue/default_reducer.h"
#include "dialogue/types.h"

namespace shizuru::core::dialogue {
namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
bool HasEffect(const std::vector<DialogueEffect>& effects) {
  for (const auto& e : effects) {
    if (std::holds_alternative<T>(e)) return true;
  }
  return false;
}

template <typename T>
const T& GetEffect(const std::vector<DialogueEffect>& effects) {
  for (const auto& e : effects) {
    if (std::holds_alternative<T>(e)) return std::get<T>(e);
  }
  throw std::runtime_error("Effect not found");
}

class DialogueReducerPhase3Test : public ::testing::Test {
 protected:
  ControllerConfig config_;
  DefaultDialogueReducer reducer_{config_};

  DialogueState MakeDefaultState() {
    DialogueState s;
    s.conversation_active = true;
    s.cooldown = CooldownPhase::kNone;
    s.deliberation = DeliberationPhase::kIdle;
    s.session_start = Clock::now();
    s.last_activity = s.session_start;
    return s;
  }

  Observation MakeUserObservation(const std::string& content) {
    Observation obs;
    obs.type = ObservationType::kUserMessage;
    obs.item = conversation::MakeHumanMessageItem("user", "", content);
    obs.timestamp = Clock::now();
    return obs;
  }
};

// ---------------------------------------------------------------------------
// 1. HandleUserMessage debounce path — BufferToWorkspace instead of RecordMemory
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, Debounce_BuffersToWorkspace) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto obs = MakeUserObservation("fragment during debounce");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, UserMessageReceived{obs, now});

  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_FALSE(HasEffect<RecordMemory>(decision.effects));
  EXPECT_FALSE(HasEffect<CommitWorkspace>(decision.effects));

  // Workspace should have the fragment.
  ASSERT_EQ(decision.next_state.workspace.user_fragments.size(), 1u);
  EXPECT_EQ(decision.next_state.workspace.user_fragments[0].item.payload.value("text", ""),
            "fragment during debounce");
}

TEST_F(DialogueReducerPhase3Test, Debounce_AccumulatesMultipleFragments) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto now = Clock::now();

  // First fragment.
  auto obs1 = MakeUserObservation("hello");
  auto d1 = reducer_.Reduce(state, UserMessageReceived{obs1, now});
  ASSERT_EQ(d1.next_state.workspace.user_fragments.size(), 1u);

  // Second fragment.
  auto obs2 = MakeUserObservation("world");
  auto d2 = reducer_.Reduce(d1.next_state, UserMessageReceived{obs2, now});
  ASSERT_EQ(d2.next_state.workspace.user_fragments.size(), 2u);
  EXPECT_EQ(d2.next_state.workspace.user_fragments[0].item.payload.value("text", ""), "hello");
  EXPECT_EQ(d2.next_state.workspace.user_fragments[1].item.payload.value("text", ""), "world");
}

// ---------------------------------------------------------------------------
// 2. HandleUserMessage normal path — BufferToWorkspace + CommitWorkspace
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, NormalPath_BuffersAndCommitsImmediately) {
  auto state = MakeDefaultState();
  auto obs = MakeUserObservation("normal message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, UserMessageReceived{obs, now});

  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<StartTurnTriggerClassification>(decision.effects));
  EXPECT_FALSE(HasEffect<RecordMemory>(decision.effects));

  // CommitWorkspace should have merge_fragments=false.
  const auto& commit = GetEffect<CommitWorkspace>(decision.effects);
  EXPECT_FALSE(commit.merge_fragments);

  // Workspace should have the entry (for effect handler to read).
  ASSERT_EQ(decision.next_state.workspace.user_fragments.size(), 1u);
}

// ---------------------------------------------------------------------------
// 3. HandleUserMessage superseding path
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, Superseding_BuffersAndCommitsNewMessage) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  state.pending_turn_trigger_id = 5;
  state.next_turn_trigger_id = 5;
  auto obs = MakeUserObservation("superseding");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, UserMessageReceived{obs, now});

  EXPECT_TRUE(HasEffect<CancelTurnTriggerClassification>(decision.effects));
  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<StartTurnTriggerClassification>(decision.effects));
  EXPECT_FALSE(HasEffect<RecordMemory>(decision.effects));
}

// ---------------------------------------------------------------------------
// 4. HandleDebounceCooldownExpired — CommitWorkspace before continuation
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, DebounceExpiry_CommitsWorkspaceBeforeContinuation) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  // Add some fragments to workspace.
  WorkspaceEntry frag1;
  frag1.item = conversation::MakeHumanMessageItem("user", "", "hello");
  frag1.timestamp = Clock::now();
  state.workspace.user_fragments.push_back(frag1);

  auto now = state.session_start + std::chrono::seconds(1);
  auto decision = reducer_.Reduce(state, DebounceCooldownExpired{now});

  // CommitWorkspace should come before StartLlmContinuation.
  ASSERT_GE(decision.effects.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<CommitWorkspace>(decision.effects[0]));
  const auto& commit = std::get<CommitWorkspace>(decision.effects[0]);
  EXPECT_TRUE(commit.merge_fragments);

  // Workspace should still be populated in next_state (for effect handler).
  ASSERT_EQ(decision.next_state.workspace.user_fragments.size(), 1u);
}

TEST_F(DialogueReducerPhase3Test, DebounceExpiry_EmptyWorkspace_StillEmitsCommit) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  // Empty workspace.

  auto now = state.session_start + std::chrono::seconds(1);
  auto decision = reducer_.Reduce(state, DebounceCooldownExpired{now});

  // CommitWorkspace is still emitted (effect handler handles empty case).
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
}

// ---------------------------------------------------------------------------
// 5. HandleInterrupt — workspace handling
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, Interrupt_WithUserFragments_CommitsBeforeCancelLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  WorkspaceEntry frag;
  frag.item = conversation::MakeHumanMessageItem("user", "", "user input");
  frag.timestamp = Clock::now();
  state.workspace.user_fragments.push_back(frag);

  auto now = Clock::now();
  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  // CommitWorkspace should come before CancelLlm.
  ASSERT_GE(decision.effects.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<CommitWorkspace>(decision.effects[0]));
  // CancelLlm should follow.
  bool found_cancel = false;
  for (size_t i = 1; i < decision.effects.size(); ++i) {
    if (std::holds_alternative<CancelLlm>(decision.effects[i])) {
      found_cancel = true;
      break;
    }
  }
  EXPECT_TRUE(found_cancel);
}

TEST_F(DialogueReducerPhase3Test, Interrupt_WithAssistantPartial_DiscardsBeforeCancelLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  WorkspaceEntry asst;
  asst.item = conversation::MakeAssistantMessageItem("assistant", "", "partial response");
  asst.timestamp = Clock::now();
  state.workspace.assistant_partial = asst;

  auto now = Clock::now();
  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  EXPECT_TRUE(HasEffect<DiscardWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CancelLlm>(decision.effects));
}

TEST_F(DialogueReducerPhase3Test, Interrupt_EmptyWorkspace_NeitherCommitNorDiscard) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  // Empty workspace.

  auto now = Clock::now();
  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  EXPECT_FALSE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_FALSE(HasEffect<DiscardWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CancelLlm>(decision.effects));
}

// ---------------------------------------------------------------------------
// 6. HandleAggregationComplete — delegates to HandleUserMessage
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, AggregationComplete_SameBehaviorAsUserMessage) {
  auto state = MakeDefaultState();
  auto obs = MakeUserObservation("aggregated message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, AggregationComplete{obs, now});

  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<StartTurnTriggerClassification>(decision.effects));
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingTurnTrigger);
}

// ---------------------------------------------------------------------------
// 7. HandleAggregationTimeout — delegates to HandleUserMessage
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, AggregationTimeout_SameBehaviorAsUserMessage) {
  auto state = MakeDefaultState();
  auto obs = MakeUserObservation("timeout-flushed");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, AggregationTimeout{obs, now});

  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<StartTurnTriggerClassification>(decision.effects));
}

// ---------------------------------------------------------------------------
// 8. HandleUserFragmentReceived — buffers without commit
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, UserFragment_BuffersWithoutCommit) {
  auto state = MakeDefaultState();
  auto obs = MakeUserObservation("partial fragment");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, UserFragmentReceived{obs, now});

  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_FALSE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_FALSE(HasEffect<StartTurnTriggerClassification>(decision.effects));

  // Fragment should be in workspace.
  ASSERT_EQ(decision.next_state.workspace.user_fragments.size(), 1u);
  EXPECT_EQ(decision.next_state.workspace.user_fragments[0].item.payload.value("text", ""),
            "partial fragment");

  // BufferToWorkspace should have is_fragment=true.
  const auto& buf = GetEffect<BufferToWorkspace>(decision.effects);
  EXPECT_TRUE(buf.is_fragment);
}

// ---------------------------------------------------------------------------
// 9. TimerExpired kAggregationTimeout — reducer returns no-op
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase3Test, AggregationTimerExpired_ReducerReturnsNoOp) {
  auto state = MakeDefaultState();
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, TimerExpired{TimerKind::kAggregationTimeout, "aggregation", now});

  EXPECT_TRUE(decision.effects.empty());
}

}  // namespace
}  // namespace shizuru::core::dialogue
