// Unit tests for DefaultDialogueReducer — Phase 2 handlers.
//
// Validates: Requirements 5.1, 5.2, 5.3, 6.1, 6.2, 6.3, 7.1–7.6,
//            8.1, 8.2, 9.1–9.4, 10.1, 10.2, 11.1, 11.2, 12.1, 12.2,
//            13.1, 14.1, 14.2, 14.3, 19.1

#include <gtest/gtest.h>

#include <chrono>

#include "controller/config.h"
#include "controller/types.h"
#include "dialogue/default_reducer.h"
#include "dialogue/types.h"

namespace shizuru::core::dialogue {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class DialogueReducerPhase2Test : public ::testing::Test {
 protected:
  ControllerConfig config_;
  DefaultDialogueReducer reducer_{config_};

  DialogueState MakeDefaultState() {
    DialogueState s;
    s.conversation_active = true;
    s.cooldown = CooldownPhase::kNone;
    s.deliberation = DeliberationPhase::kIdle;
    s.turn_llm_calls = 0;
    s.turn_prompt_tokens = 0;
    s.turn_completion_tokens = 0;
    s.turn_action_count = 0;
    s.session_start = Clock::now();
    s.last_activity = s.session_start;
    s.next_turn_trigger_id = 0;
    s.pending_turn_trigger_id = 0;
    return s;
  }

  Observation MakeUserObservation(const std::string& content) {
    Observation obs;
    obs.type = ObservationType::kUserMessage;
    obs.content = content;
    obs.source = "user";
    obs.timestamp = Clock::now();
    return obs;
  }

  Observation MakeSystemObservation(const std::string& content) {
    Observation obs;
    obs.type = ObservationType::kSystemEvent;
    obs.content = content;
    obs.source = "scheduler";
    obs.timestamp = Clock::now();
    return obs;
  }

  Observation MakeToolResultObservation(const std::string& call_id,
                                        const std::string& result) {
    Observation obs;
    obs.type = ObservationType::kToolResult;
    obs.content = result;
    obs.source = call_id;
    obs.timestamp = Clock::now();
    return obs;
  }

  // Helper: check if any effect in the list holds the given alternative.
  template <typename T>
  bool HasEffect(const std::vector<DialogueEffect>& effects) {
    for (const auto& e : effects) {
      if (std::holds_alternative<T>(e)) return true;
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// 1. HandleUserMessage normal path (cooldown=kNone, deliberation=kIdle)
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       UserMessage_NormalPath_EmitsRecordMemoryAndStartTurnTrigger) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kNone;
  state.deliberation = DeliberationPhase::kIdle;
  auto obs = MakeUserObservation("hello");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, UserMessageReceived{obs, now});

  // Phase 3: BufferToWorkspace + CommitWorkspace instead of RecordMemory.
  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<StartTurnTriggerClassification>(decision.effects));
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingTurnTrigger);
  EXPECT_TRUE(decision.next_state.conversation_active);
  EXPECT_EQ(decision.next_state.last_activity, now);
}

// ---------------------------------------------------------------------------
// 2. HandleUserMessage with idle timeout
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       UserMessage_NormalPath_ResetsPerTurnCounters) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kNone;
  state.deliberation = DeliberationPhase::kIdle;
  state.turn_llm_calls = 5;
  state.turn_prompt_tokens = 1000;
  state.turn_completion_tokens = 500;
  state.turn_action_count = 10;
  state.turn_continuation_count = 3;

  auto obs = MakeUserObservation("new message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, UserMessageReceived{obs, now});

  // Per-turn counters should be reset for the new turn.
  EXPECT_EQ(decision.next_state.turn_llm_calls, 0);
  EXPECT_EQ(decision.next_state.turn_prompt_tokens, 0);
  EXPECT_EQ(decision.next_state.turn_completion_tokens, 0);
  EXPECT_EQ(decision.next_state.turn_action_count, 0);
  EXPECT_EQ(decision.next_state.turn_continuation_count, 0);
}

// ---------------------------------------------------------------------------
// 3. HandleTurnTriggerClassified — matching obs_id + kRespondNow
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       TurnTriggerClassified_RespondNow_SetsThinkingAndStartsLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  state.pending_turn_trigger_id = 42;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state,
      TurnTriggerClassified{42, TurnTriggerVerdict::kRespondNow, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  EXPECT_TRUE(HasEffect<StartLlm>(decision.effects));
}

// ---------------------------------------------------------------------------
// 4. HandleTurnTriggerClassified — matching obs_id + kStoreOnly
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       TurnTriggerClassified_StoreOnly_SetsIdleAndEmptyEffects) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  state.pending_turn_trigger_id = 42;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state,
      TurnTriggerClassified{42, TurnTriggerVerdict::kStoreOnly, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kIdle);
  // kStoreOnly now emits EmitActivityEffect{kInputStored}.
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(HasEffect<EmitActivityEffect>(decision.effects));
}

// ---------------------------------------------------------------------------
// 5. HandleTurnTriggerClassified — non-matching obs_id (stale rejection)
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       TurnTriggerClassified_StaleMismatch_NoOp) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  state.pending_turn_trigger_id = 42;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state,
      TurnTriggerClassified{99, TurnTriggerVerdict::kRespondNow, now});

  // State unchanged, no effects.
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingTurnTrigger);
  EXPECT_EQ(decision.next_state.pending_turn_trigger_id, 42u);
  EXPECT_TRUE(decision.effects.empty());
}

// ---------------------------------------------------------------------------
// 6. HandleLlmCompleted — kToolCall
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       LlmCompleted_ToolCall_UpdatesStateAndEmitsToolCallFrames) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  state.turn_llm_calls = 3;
  state.turn_prompt_tokens = 100;
  state.turn_completion_tokens = 50;

  ActionCandidate candidate;
  candidate.type = ActionType::kToolCall;
  candidate.tool_calls = {
      ToolCall{"call_a", "web_search", R"({"q":"test"})", ""},
      ToolCall{"call_b", "calculator", R"({"expr":"1+1"})", ""},
  };
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, LlmCompleted{candidate, 200, 80, now});

  // Turn count incremented.
  EXPECT_EQ(decision.next_state.turn_llm_calls, 4);
  // Tokens accumulated.
  EXPECT_EQ(decision.next_state.turn_prompt_tokens, 300);
  EXPECT_EQ(decision.next_state.turn_completion_tokens, 130);
  // Deliberation transitions to awaiting tool results.
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingToolResults);
  // Pending tool call ids populated.
  ASSERT_EQ(decision.next_state.pending_tool_call_ids.size(), 2u);
  EXPECT_EQ(decision.next_state.pending_tool_call_ids[0], "call_a");
  EXPECT_EQ(decision.next_state.pending_tool_call_ids[1], "call_b");
  // Effects contain EmitToolCallFrames.
  EXPECT_TRUE(HasEffect<EmitToolCallFrames>(decision.effects));
}

// ---------------------------------------------------------------------------
// 7. HandleLlmCompleted — kResponse
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       LlmCompleted_Response_DeliversResponseAndSetsIdle) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  state.turn_llm_calls = 2;

  ActionCandidate candidate;
  candidate.type = ActionType::kResponse;
  candidate.response_text = "Hello there!";
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, LlmCompleted{candidate, 50, 30, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kIdle);
  EXPECT_TRUE(HasEffect<DeliverResponse>(decision.effects));
  EXPECT_EQ(decision.next_state.turn_llm_calls, 3);
}

// ---------------------------------------------------------------------------
// 8. HandleLlmCompleted — kContinue
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       LlmCompleted_Continue_StartsLlmAndStaysThinking) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  state.turn_llm_calls = 1;

  ActionCandidate candidate;
  candidate.type = ActionType::kContinue;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, LlmCompleted{candidate, 40, 20, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  EXPECT_TRUE(HasEffect<StartLlm>(decision.effects));
  EXPECT_EQ(decision.next_state.turn_llm_calls, 2);
}

// ---------------------------------------------------------------------------
// 9. HandleLlmFailed
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       LlmFailed_SetsIdleAndEmitsDiagnosticAndTransition) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, LlmFailed{"timeout", now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kIdle);
  ASSERT_EQ(decision.effects.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<EmitDiagnosticEffect>(
      decision.effects[0]));
  EXPECT_TRUE(std::holds_alternative<TransitionState>(decision.effects[1]));
}

// ---------------------------------------------------------------------------
// 10. HandleToolResult — partial (not all results in)
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       ToolResult_Partial_RecordsResultAndStaysAwaiting) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;
  state.pending_tool_call_ids = {"call_1", "call_2"};

  auto obs = MakeToolResultObservation("call_1", R"({"result":"ok"})");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, ToolResultReceived{obs, now});

  // Result recorded.
  EXPECT_TRUE(HasEffect<RecordToolResult>(decision.effects));
  EXPECT_EQ(decision.next_state.pending_tool_results.count("call_1"), 1u);
  // Still awaiting the second result.
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingToolResults);
  // No StartLlm yet.
  EXPECT_FALSE(HasEffect<StartLlm>(decision.effects));
}

// ---------------------------------------------------------------------------
// 11. HandleToolResult — complete (all results in)
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       ToolResult_Complete_TransitionsToThinkingAndStartsLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;
  state.pending_tool_call_ids = {"call_1"};

  auto obs = MakeToolResultObservation("call_1", R"({"result":"done"})");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, ToolResultReceived{obs, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  EXPECT_TRUE(decision.next_state.pending_tool_call_ids.empty());
  EXPECT_TRUE(decision.next_state.pending_tool_results.empty());
  EXPECT_TRUE(HasEffect<StartLlm>(decision.effects));
  // Bug fix: CancelTimer must be emitted to prevent stale timeout from
  // firing after all tool results have arrived.
  EXPECT_TRUE(HasEffect<CancelTimer>(decision.effects));
}

// ---------------------------------------------------------------------------
// 12. HandleToolCallTimeout
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       ToolCallTimeout_EmitsRecordTimeoutAndStartsLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;
  state.pending_tool_call_ids = {"call_1", "call_2"};
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, ToolCallTimeout{{"call_1", "call_2"}, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  EXPECT_TRUE(decision.next_state.pending_tool_call_ids.empty());
  EXPECT_TRUE(decision.next_state.pending_tool_results.empty());

  EXPECT_TRUE(HasEffect<RecordTimeoutResults>(decision.effects));
  EXPECT_TRUE(HasEffect<StartLlm>(decision.effects));

  // Verify the missing ids are carried in the effect.
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<RecordTimeoutResults>(e)) {
      const auto& rtr = std::get<RecordTimeoutResults>(e);
      ASSERT_EQ(rtr.missing_tool_call_ids.size(), 2u);
      EXPECT_EQ(rtr.missing_tool_call_ids[0], "call_1");
      EXPECT_EQ(rtr.missing_tool_call_ids[1], "call_2");
    }
  }
}

// ---------------------------------------------------------------------------
// 13. HandleInterrupt with deliberation=kAwaitingToolResults
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       Interrupt_AwaitingToolResults_EmitsCancelTimer) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;
  state.pending_tool_call_ids = {"call_1"};
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  EXPECT_TRUE(HasEffect<CancelTimer>(decision.effects));
  // Also verify standard interrupt effects.
  EXPECT_TRUE(HasEffect<CancelLlm>(decision.effects));
  EXPECT_TRUE(HasEffect<RecordInterruptMemory>(decision.effects));
  EXPECT_TRUE(HasEffect<ScheduleTimer>(decision.effects));
}

// ---------------------------------------------------------------------------
// 14. HandleUserMessage superseding during kAwaitingTurnTrigger
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       UserMessage_Superseding_CancelsOldAndStartsNewTrigger) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kNone;
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  state.pending_turn_trigger_id = 5;
  state.next_turn_trigger_id = 5;

  auto obs = MakeUserObservation("superseding message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, UserMessageReceived{obs, now});

  // Must cancel the old turn-trigger classification.
  EXPECT_TRUE(HasEffect<CancelTurnTriggerClassification>(decision.effects));
  // Phase 3: BufferToWorkspace + CommitWorkspace instead of RecordMemory.
  EXPECT_TRUE(HasEffect<BufferToWorkspace>(decision.effects));
  EXPECT_TRUE(HasEffect<CommitWorkspace>(decision.effects));
  // Must start a new turn-trigger classification with a fresh id.
  EXPECT_TRUE(HasEffect<StartTurnTriggerClassification>(decision.effects));
  // The pending_turn_trigger_id must have changed.
  EXPECT_NE(decision.next_state.pending_turn_trigger_id,
            state.pending_turn_trigger_id);
  // Deliberation stays kAwaitingTurnTrigger.
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingTurnTrigger);
}

// ---------------------------------------------------------------------------
// 15. HandleTimerExpired kDebounce — equivalent to DebounceCooldownExpired
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       TimerExpired_Debounce_EquivalentToDebounceCooldownExpired) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto now = state.session_start + std::chrono::seconds(1);

  auto via_timer = reducer_.Reduce(
      state, TimerExpired{TimerKind::kDebounce, "debounce", now});
  auto via_direct = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  // State fields must match.
  EXPECT_EQ(via_timer.next_state.cooldown, via_direct.next_state.cooldown);
  EXPECT_EQ(via_timer.next_state.turn_llm_calls,
            via_direct.next_state.turn_llm_calls);
  EXPECT_EQ(via_timer.next_state.deliberation,
            via_direct.next_state.deliberation);

  // Effects must match in count and type.
  ASSERT_EQ(via_timer.effects.size(), via_direct.effects.size());
  for (size_t i = 0; i < via_timer.effects.size(); ++i) {
    EXPECT_EQ(via_timer.effects[i].index(), via_direct.effects[i].index());
  }
}

// ---------------------------------------------------------------------------
// 16. HandleSystemEvent
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       SystemEvent_EmitsRecordMemoryAndStartLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kIdle;

  auto obs = MakeSystemObservation("reminder");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, SystemEventReceived{obs, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  EXPECT_TRUE(decision.next_state.conversation_active);
  EXPECT_TRUE(HasEffect<RecordMemory>(decision.effects));
  EXPECT_TRUE(HasEffect<StartLlm>(decision.effects));
}

// ---------------------------------------------------------------------------
// 17. HandleContinuation
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerPhase2Test,
       Continuation_EmitsStartLlmAndSetsThinking) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kIdle;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, ContinuationRequested{"test", now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlm>(decision.effects[0]));
}

}  // namespace
}  // namespace shizuru::core::dialogue
