// Unit tests for DefaultDialogueReducer — barge-in, debounce, budget.
//
// Validates: Requirements 12.1, 12.5, 13.1

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

class DialogueReducerTest : public ::testing::Test {
 protected:
  ControllerConfig config_;
  DefaultDialogueReducer reducer_{config_};

  DialogueState MakeDefaultState() {
    DialogueState s;
    s.conversation_active = true;
    s.cooldown = CooldownPhase::kNone;
    s.turn_llm_calls = 0;
    s.turn_prompt_tokens = 0;
    s.turn_completion_tokens = 0;
    s.turn_action_count = 0;
    s.turn_continuation_count = 0;
    s.session_start = Clock::now();
    s.last_activity = s.session_start;
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
};

// ---------------------------------------------------------------------------
// 1. HandleInterrupt
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, HandleInterrupt_SetsCooldownToDebouncing) {
  auto state = MakeDefaultState();
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  EXPECT_EQ(decision.next_state.cooldown, CooldownPhase::kDebouncing);
}

TEST_F(DialogueReducerTest, HandleInterrupt_EmitsCancelLlmAndRecordInterruptMemoryAndScheduleTimer) {
  auto state = MakeDefaultState();
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  // Phase 2: CancelLlm + RecordInterruptMemory + ScheduleTimer{kDebounce}
  ASSERT_GE(decision.effects.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<CancelLlm>(decision.effects[0]));
  EXPECT_TRUE(std::holds_alternative<RecordInterruptMemory>(decision.effects[1]));
  EXPECT_TRUE(std::holds_alternative<ScheduleTimer>(decision.effects[2]));
}

TEST_F(DialogueReducerTest, HandleInterrupt_NoSignalInterruptedEffect) {
  auto state = MakeDefaultState();
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  for (const auto& effect : decision.effects) {
    // SignalBudgetExhausted is the closest "signal" type — ensure no
    // unexpected signal effects are emitted.
    EXPECT_FALSE(std::holds_alternative<SignalBudgetExhausted>(effect));
    EXPECT_FALSE(std::holds_alternative<EmitActivityEffect>(effect));
  }
}

TEST_F(DialogueReducerTest, HandleInterrupt_UpdatesLastActivityToNow) {
  auto state = MakeDefaultState();
  auto now = state.session_start + std::chrono::seconds(5);

  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  EXPECT_EQ(decision.next_state.last_activity, now);
}

// ---------------------------------------------------------------------------
// 2. HandleUserMessage during debounce
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, UserMessageDuringDebounce_EmitsRecordMemory) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto obs = MakeUserObservation("hello during debounce");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, UserMessageReceived{obs, now});

  // Phase 3: BufferToWorkspace instead of RecordMemory during debounce.
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<BufferToWorkspace>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, UserMessageDuringDebounce_NoStartLlmContinuation) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto obs = MakeUserObservation("buffered message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, UserMessageReceived{obs, now});

  for (const auto& effect : decision.effects) {
    EXPECT_FALSE(std::holds_alternative<StartLlmContinuation>(effect));
  }
}

TEST_F(DialogueReducerTest, UserMessageDuringDebounce_CooldownStaysDebouncing) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto obs = MakeUserObservation("still debouncing");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, UserMessageReceived{obs, now});

  EXPECT_EQ(decision.next_state.cooldown, CooldownPhase::kDebouncing);
}

// ---------------------------------------------------------------------------
// 3. HandleUserMessage when cooldown is kNone — Phase 2 normal path
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, UserMessageNoCooldown_EmitsRecordMemoryAndStartTurnTrigger) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kNone;
  state.deliberation = DeliberationPhase::kIdle;
  auto obs = MakeUserObservation("normal message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, UserMessageReceived{obs, now});

  // Phase 3: BufferToWorkspace + CommitWorkspace + StartTurnTriggerClassification
  EXPECT_FALSE(decision.effects.empty());
  bool has_buffer = false;
  bool has_commit = false;
  bool has_trigger = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<BufferToWorkspace>(effect)) has_buffer = true;
    if (std::holds_alternative<CommitWorkspace>(effect)) has_commit = true;
    if (std::holds_alternative<StartTurnTriggerClassification>(effect))
      has_trigger = true;
  }
  EXPECT_TRUE(has_buffer);
  EXPECT_TRUE(has_commit);
  EXPECT_TRUE(has_trigger);
  EXPECT_EQ(decision.next_state.deliberation,
            DeliberationPhase::kAwaitingTurnTrigger);
  EXPECT_TRUE(decision.next_state.conversation_active);
}

// ---------------------------------------------------------------------------
// 4. HandleDebounceCooldownExpired with budget OK
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, DebounceCooldownExpired_BudgetOk_ClearsCooldown) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  EXPECT_EQ(decision.next_state.cooldown, CooldownPhase::kNone);
}

TEST_F(DialogueReducerTest, DebounceCooldownExpired_BudgetOk_EmitsStartLlmContinuation) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  // Phase 3: CommitWorkspace{true} + StartLlmContinuation
  ASSERT_EQ(decision.effects.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<CommitWorkspace>(decision.effects[0]));
  EXPECT_TRUE(std::holds_alternative<StartLlmContinuation>(decision.effects[1]));
}

// ---------------------------------------------------------------------------
// 5. HandleDebounceCooldownExpired with budget exhausted
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, DebounceCooldownExpired_BudgetExhausted_EmitsSignalBudgetExhausted) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_action_count = config_.action_count_limit;  // At the limit.
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  // Phase 3: CommitWorkspace{true} + SignalBudgetExhausted
  ASSERT_EQ(decision.effects.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<CommitWorkspace>(decision.effects[0]));
  EXPECT_TRUE(std::holds_alternative<SignalBudgetExhausted>(decision.effects[1]));
}

TEST_F(DialogueReducerTest, DebounceCooldownExpired_BudgetExhausted_NoStartLlmContinuation) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_action_count = config_.action_count_limit;
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  for (const auto& effect : decision.effects) {
    EXPECT_FALSE(std::holds_alternative<StartLlmContinuation>(effect));
  }
}

// ---------------------------------------------------------------------------
// 6. IsBudgetExhausted boundary values (per-turn limits)
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, BudgetBoundary_TokensAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.token_budget = 1000;
  cfg.action_count_limit = 50;
  cfg.max_continuations = 50;
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_prompt_tokens = 600;
  state.turn_completion_tokens = 400;  // sum == 1000 == token_budget
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  bool has_exhausted = false;
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<SignalBudgetExhausted>(e)) has_exhausted = true;
  }
  EXPECT_TRUE(has_exhausted);
}

TEST_F(DialogueReducerTest, BudgetBoundary_TokensBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.token_budget = 1000;
  cfg.action_count_limit = 50;
  cfg.max_continuations = 50;
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_prompt_tokens = 500;
  state.turn_completion_tokens = 499;  // sum == 999 < token_budget
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  bool has_continuation = false;
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<StartLlmContinuation>(e)) has_continuation = true;
  }
  EXPECT_TRUE(has_continuation);
}

TEST_F(DialogueReducerTest, BudgetBoundary_ActionsAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 10;
  cfg.max_continuations = 50;
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_action_count = 10;  // == action_count_limit
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  bool has_exhausted = false;
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<SignalBudgetExhausted>(e)) has_exhausted = true;
  }
  EXPECT_TRUE(has_exhausted);
}

TEST_F(DialogueReducerTest, BudgetBoundary_ActionsBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 10;
  cfg.max_continuations = 50;
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_action_count = 9;  // == action_count_limit - 1
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  bool has_continuation = false;
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<StartLlmContinuation>(e)) has_continuation = true;
  }
  EXPECT_TRUE(has_continuation);
}

TEST_F(DialogueReducerTest, BudgetBoundary_ContinuationsAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.max_continuations = 3;
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_continuation_count = 3;  // == max_continuations
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  bool has_exhausted = false;
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<SignalBudgetExhausted>(e)) has_exhausted = true;
  }
  EXPECT_TRUE(has_exhausted);
}

TEST_F(DialogueReducerTest, BudgetBoundary_ContinuationsBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.max_continuations = 3;
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_continuation_count = 2;  // == max_continuations - 1
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  bool has_continuation = false;
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<StartLlmContinuation>(e)) has_continuation = true;
  }
  EXPECT_TRUE(has_continuation);
}

// ---------------------------------------------------------------------------
// 7. ShutdownRequested — state unchanged, empty effects
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, ShutdownRequested_StateUnchanged) {
  auto state = MakeDefaultState();
  state.turn_llm_calls = 3;
  state.cooldown = CooldownPhase::kDebouncing;

  auto decision = reducer_.Reduce(state, ShutdownRequested{});

  EXPECT_TRUE(decision.effects.empty());
  EXPECT_EQ(decision.next_state.cooldown, state.cooldown);
  EXPECT_EQ(decision.next_state.turn_llm_calls, state.turn_llm_calls);
  EXPECT_EQ(decision.next_state.conversation_active, state.conversation_active);
}

// ---------------------------------------------------------------------------
// 8. Phase 2 event handlers — verify they produce real effects
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, LlmCompleted_IncrementsTurnCount) {
  auto state = MakeDefaultState();
  state.turn_llm_calls = 7;
  state.deliberation = DeliberationPhase::kThinking;

  ActionCandidate candidate;
  candidate.type = ActionType::kResponse;
  candidate.response_text = "hello";
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, LlmCompleted{candidate, 100, 50, now});

  EXPECT_EQ(decision.next_state.turn_llm_calls, 8);
  EXPECT_FALSE(decision.effects.empty());
}

TEST_F(DialogueReducerTest, LlmFailed_EmitsDiagnosticAndTransition) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kThinking;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, LlmFailed{"timeout", now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kIdle);
  ASSERT_EQ(decision.effects.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<EmitDiagnosticEffect>(decision.effects[0]));
  EXPECT_TRUE(std::holds_alternative<TransitionState>(decision.effects[1]));
}

TEST_F(DialogueReducerTest, ToolResultReceived_RecordsResult) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;
  state.pending_tool_call_ids = {"call_1"};

  Observation obs;
  obs.type = ObservationType::kToolResult;
  obs.content = R"({"result":"ok"})";
  obs.source = "call_1";
  auto now = Clock::now();
  obs.timestamp = now;

  auto decision = reducer_.Reduce(
      state, ToolResultReceived{obs, now});

  EXPECT_FALSE(decision.effects.empty());
  bool has_record = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<RecordToolResult>(effect)) has_record = true;
  }
  EXPECT_TRUE(has_record);
}

TEST_F(DialogueReducerTest, ToolCallTimeout_EmitsRecordTimeoutAndStartLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;
  state.pending_tool_call_ids = {"call_1"};
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, ToolCallTimeout{{"call_1"}, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  bool has_timeout = false;
  bool has_start = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<RecordTimeoutResults>(effect)) has_timeout = true;
    if (std::holds_alternative<StartLlm>(effect)) has_start = true;
  }
  EXPECT_TRUE(has_timeout);
  EXPECT_TRUE(has_start);
}

TEST_F(DialogueReducerTest, ContinuationRequested_EmitsStartLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kIdle;
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, ContinuationRequested{"test", now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlm>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, SystemEventReceived_EmitsRecordMemoryAndStartLlm) {
  auto state = MakeDefaultState();
  state.deliberation = DeliberationPhase::kIdle;

  Observation obs;
  obs.type = ObservationType::kSystemEvent;
  obs.content = "reminder";
  obs.source = "scheduler";
  auto now = Clock::now();
  obs.timestamp = now;

  auto decision = reducer_.Reduce(
      state, SystemEventReceived{obs, now});

  EXPECT_EQ(decision.next_state.deliberation, DeliberationPhase::kThinking);
  EXPECT_TRUE(decision.next_state.conversation_active);
  bool has_record = false;
  bool has_start = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<RecordMemory>(effect)) has_record = true;
    if (std::holds_alternative<StartLlm>(effect)) has_start = true;
  }
  EXPECT_TRUE(has_record);
  EXPECT_TRUE(has_start);
}

// ---------------------------------------------------------------------------
// 9. Reducer purity — same inputs produce identical outputs
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, Purity_SameInputsSameOutputs) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  auto now = state.session_start + std::chrono::seconds(2);

  auto decision1 = reducer_.Reduce(state, DebounceCooldownExpired{now});
  auto decision2 = reducer_.Reduce(state, DebounceCooldownExpired{now});

  // State fields must match.
  EXPECT_EQ(decision1.next_state.cooldown, decision2.next_state.cooldown);
  EXPECT_EQ(decision1.next_state.turn_llm_calls, decision2.next_state.turn_llm_calls);
  EXPECT_EQ(decision1.next_state.conversation_active,
            decision2.next_state.conversation_active);
  EXPECT_EQ(decision1.next_state.last_activity, decision2.next_state.last_activity);

  // Effects must match in count and type.
  ASSERT_EQ(decision1.effects.size(), decision2.effects.size());
  for (size_t i = 0; i < decision1.effects.size(); ++i) {
    EXPECT_EQ(decision1.effects[i].index(), decision2.effects[i].index());
  }
}

TEST_F(DialogueReducerTest, Purity_InterruptSameInputsSameOutputs) {
  auto state = MakeDefaultState();
  auto now = state.session_start + std::chrono::seconds(1);

  auto d1 = reducer_.Reduce(state, InterruptRequested{now});
  auto d2 = reducer_.Reduce(state, InterruptRequested{now});

  EXPECT_EQ(d1.next_state.cooldown, d2.next_state.cooldown);
  EXPECT_EQ(d1.next_state.last_activity, d2.next_state.last_activity);
  ASSERT_EQ(d1.effects.size(), d2.effects.size());
  for (size_t i = 0; i < d1.effects.size(); ++i) {
    EXPECT_EQ(d1.effects[i].index(), d2.effects[i].index());
  }
}

}  // namespace
}  // namespace shizuru::core::dialogue
