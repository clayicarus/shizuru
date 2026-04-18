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
    s.turn_count = 0;
    s.total_prompt_tokens = 0;
    s.total_completion_tokens = 0;
    s.action_count = 0;
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

TEST_F(DialogueReducerTest, HandleInterrupt_EmitsSingleCancelLlmEffect) {
  auto state = MakeDefaultState();
  auto now = Clock::now();

  auto decision = reducer_.Reduce(state, InterruptRequested{now});

  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<CancelLlm>(decision.effects[0]));
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

  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<RecordMemory>(decision.effects[0]));
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
// 3. HandleUserMessage when cooldown is kNone — no-op for Phase 1
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, UserMessageNoCooldown_IsNoOp) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kNone;
  auto obs = MakeUserObservation("normal message");
  auto now = Clock::now();

  auto decision = reducer_.Reduce(
      state, UserMessageReceived{obs, now});

  EXPECT_TRUE(decision.effects.empty());
  // State should be unchanged.
  EXPECT_EQ(decision.next_state.cooldown, state.cooldown);
  EXPECT_EQ(decision.next_state.turn_count, state.turn_count);
  EXPECT_EQ(decision.next_state.conversation_active, state.conversation_active);
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

  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlmContinuation>(decision.effects[0]));
}

// ---------------------------------------------------------------------------
// 5. HandleDebounceCooldownExpired with budget exhausted
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, DebounceCooldownExpired_BudgetExhausted_EmitsSignalBudgetExhausted) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_count = config_.max_turns;  // At the limit.
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SignalBudgetExhausted>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, DebounceCooldownExpired_BudgetExhausted_NoStartLlmContinuation) {
  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_count = config_.max_turns;
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = reducer_.Reduce(
      state, DebounceCooldownExpired{now});

  for (const auto& effect : decision.effects) {
    EXPECT_FALSE(std::holds_alternative<StartLlmContinuation>(effect));
  }
}

// ---------------------------------------------------------------------------
// 6. IsBudgetExhausted boundary values
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, BudgetBoundary_TurnsAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 5;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.turn_timeout = std::chrono::seconds(60);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_count = 5;  // == max_turns
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SignalBudgetExhausted>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_TurnsBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 5;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.turn_timeout = std::chrono::seconds(60);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.turn_count = 4;  // == max_turns - 1
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlmContinuation>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_TokensAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.token_budget = 1000;
  cfg.action_count_limit = 50;
  cfg.turn_timeout = std::chrono::seconds(60);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.total_prompt_tokens = 600;
  state.total_completion_tokens = 400;  // sum == 1000 == token_budget
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SignalBudgetExhausted>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_TokensBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.token_budget = 1000;
  cfg.action_count_limit = 50;
  cfg.turn_timeout = std::chrono::seconds(60);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.total_prompt_tokens = 500;
  state.total_completion_tokens = 499;  // sum == 999 < token_budget
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlmContinuation>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_ActionsAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 10;
  cfg.turn_timeout = std::chrono::seconds(60);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.action_count = 10;  // == action_count_limit
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SignalBudgetExhausted>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_ActionsBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 10;
  cfg.turn_timeout = std::chrono::seconds(60);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  state.action_count = 9;  // == action_count_limit - 1
  auto now = state.session_start + std::chrono::seconds(1);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlmContinuation>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_TimeAtLimit_Exhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.turn_timeout = std::chrono::seconds(30);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  // now - session_start == turn_timeout exactly
  auto now = state.session_start + std::chrono::seconds(30);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<SignalBudgetExhausted>(decision.effects[0]));
}

TEST_F(DialogueReducerTest, BudgetBoundary_TimeBelowLimit_NotExhausted) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  cfg.turn_timeout = std::chrono::seconds(30);
  DefaultDialogueReducer r(cfg);

  auto state = MakeDefaultState();
  state.cooldown = CooldownPhase::kDebouncing;
  // now - session_start == turn_timeout - 1s
  auto now = state.session_start + std::chrono::seconds(29);

  auto decision = r.Reduce(state, DebounceCooldownExpired{now});
  ASSERT_EQ(decision.effects.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<StartLlmContinuation>(decision.effects[0]));
}

// ---------------------------------------------------------------------------
// 7. ShutdownRequested — state unchanged, empty effects
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, ShutdownRequested_StateUnchanged) {
  auto state = MakeDefaultState();
  state.turn_count = 3;
  state.cooldown = CooldownPhase::kDebouncing;

  auto decision = reducer_.Reduce(state, ShutdownRequested{});

  EXPECT_TRUE(decision.effects.empty());
  EXPECT_EQ(decision.next_state.cooldown, state.cooldown);
  EXPECT_EQ(decision.next_state.turn_count, state.turn_count);
  EXPECT_EQ(decision.next_state.conversation_active, state.conversation_active);
}

// ---------------------------------------------------------------------------
// 8. Unhandled event types — no-op passthrough
// ---------------------------------------------------------------------------

TEST_F(DialogueReducerTest, UnhandledEvent_LlmCompleted_NoOp) {
  auto state = MakeDefaultState();
  state.turn_count = 7;

  auto decision = reducer_.Reduce(state, LlmCompleted{});

  EXPECT_TRUE(decision.effects.empty());
  EXPECT_EQ(decision.next_state.turn_count, state.turn_count);
}

TEST_F(DialogueReducerTest, UnhandledEvent_LlmFailed_NoOp) {
  auto state = MakeDefaultState();

  auto decision = reducer_.Reduce(state, LlmFailed{});

  EXPECT_TRUE(decision.effects.empty());
}

TEST_F(DialogueReducerTest, UnhandledEvent_ToolResultReceived_NoOp) {
  auto state = MakeDefaultState();

  auto decision = reducer_.Reduce(state, ToolResultReceived{});

  EXPECT_TRUE(decision.effects.empty());
}

TEST_F(DialogueReducerTest, UnhandledEvent_ToolCallTimeout_NoOp) {
  auto state = MakeDefaultState();

  auto decision = reducer_.Reduce(state, ToolCallTimeout{});

  EXPECT_TRUE(decision.effects.empty());
}

TEST_F(DialogueReducerTest, UnhandledEvent_ContinuationRequested_NoOp) {
  auto state = MakeDefaultState();

  auto decision = reducer_.Reduce(state, ContinuationRequested{});

  EXPECT_TRUE(decision.effects.empty());
}

TEST_F(DialogueReducerTest, UnhandledEvent_SystemEventReceived_NoOp) {
  auto state = MakeDefaultState();

  auto decision = reducer_.Reduce(state, SystemEventReceived{});

  EXPECT_TRUE(decision.effects.empty());
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
  EXPECT_EQ(decision1.next_state.turn_count, decision2.next_state.turn_count);
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
