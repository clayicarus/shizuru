// Property-based tests for DefaultDialogueReducer
// Uses RapidCheck + Google Test
//
// Validates: Requirements 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 6.1, 6.2, 6.3

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <chrono>

#include "controller/config.h"
#include "controller/types.h"
#include "dialogue/default_reducer.h"
#include "dialogue/types.h"

namespace shizuru::core::dialogue {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using namespace std::chrono_literals;

// Fixed base time for deterministic generation.
static const TimePoint kBaseTime = Clock::now();

// ---------------------------------------------------------------------------
// RapidCheck generators
// ---------------------------------------------------------------------------

rc::Gen<DialogueState> genDialogueState() {
  return rc::gen::exec([] {
    DialogueState s;
    s.conversation_active = *rc::gen::arbitrary<bool>();
    s.cooldown = *rc::gen::element(CooldownPhase::kNone,
                                   CooldownPhase::kDebouncing);
    s.turn_count = *rc::gen::inRange(0, 50);
    s.total_prompt_tokens = *rc::gen::inRange(0, 50000);
    s.total_completion_tokens = *rc::gen::inRange(0, 50000);
    s.action_count = *rc::gen::inRange(0, 100);
    s.session_start = kBaseTime;
    s.last_activity =
        kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));
    return s;
  });
}

rc::Gen<DialogueEvent> genDialogueEvent() {
  return rc::gen::exec([] {
    int choice = *rc::gen::inRange(0, 10);
    auto offset = std::chrono::seconds(*rc::gen::inRange(0, 3600));
    auto now = kBaseTime + offset;

    switch (choice) {
      case 0:
        return DialogueEvent{InterruptRequested{now}};
      case 1:
        return DialogueEvent{DebounceCooldownExpired{now}};
      case 2: {
        Observation obs;
        obs.type = ObservationType::kUserMessage;
        obs.content = "test";
        obs.source = "user";
        obs.timestamp = now;
        return DialogueEvent{UserMessageReceived{obs, now}};
      }
      case 3:
        return DialogueEvent{ShutdownRequested{}};
      case 4:
        return DialogueEvent{LlmCompleted{}};
      case 5:
        return DialogueEvent{LlmFailed{}};
      case 6:
        return DialogueEvent{ToolResultReceived{}};
      case 7:
        return DialogueEvent{ToolCallTimeout{}};
      case 8:
        return DialogueEvent{ContinuationRequested{}};
      case 9:
      default:
        return DialogueEvent{SystemEventReceived{}};
    }
  });
}

rc::Gen<ControllerConfig> genControllerConfig() {
  return rc::gen::exec([] {
    ControllerConfig cfg;
    cfg.max_turns = *rc::gen::inRange(1, 50);
    cfg.token_budget = *rc::gen::inRange(1000, 100000);
    cfg.action_count_limit = *rc::gen::inRange(1, 100);
    cfg.turn_timeout = std::chrono::seconds(60);
    return cfg;
  });
}

// ---------------------------------------------------------------------------
// Property 1 (Task 6.2): Reducer purity
// For any valid DialogueState and DialogueEvent, calling Reduce twice with
// identical inputs produces identical outputs.
// **Validates: Requirements 3.4, 3.5**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_reducer_purity, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto event = *genDialogueEvent();

  auto decision1 = reducer.Reduce(state, event);
  auto decision2 = reducer.Reduce(state, event);

  // Next state fields must be identical.
  RC_ASSERT(decision1.next_state.conversation_active ==
            decision2.next_state.conversation_active);
  RC_ASSERT(decision1.next_state.cooldown == decision2.next_state.cooldown);
  RC_ASSERT(decision1.next_state.turn_count == decision2.next_state.turn_count);
  RC_ASSERT(decision1.next_state.total_prompt_tokens ==
            decision2.next_state.total_prompt_tokens);
  RC_ASSERT(decision1.next_state.total_completion_tokens ==
            decision2.next_state.total_completion_tokens);
  RC_ASSERT(decision1.next_state.action_count ==
            decision2.next_state.action_count);
  RC_ASSERT(decision1.next_state.session_start ==
            decision2.next_state.session_start);
  RC_ASSERT(decision1.next_state.last_activity ==
            decision2.next_state.last_activity);

  // Effects must match in count and variant indices.
  RC_ASSERT(decision1.effects.size() == decision2.effects.size());
  for (size_t i = 0; i < decision1.effects.size(); ++i) {
    RC_ASSERT(decision1.effects[i].index() == decision2.effects[i].index());
  }
}

// ---------------------------------------------------------------------------
// Property 2 (Task 6.3): Barge-in enters debounce
// For any DialogueState where cooldown == kNone,
// Reduce(state, InterruptRequested{now}) produces
// next_state.cooldown == kDebouncing and effects contain exactly one CancelLlm.
// **Validates: Requirements 4.1**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_bargein_enters_debounce, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kNone;  // Precondition

  auto offset = std::chrono::seconds(*rc::gen::inRange(0, 3600));
  auto now = kBaseTime + offset;

  auto decision = reducer.Reduce(state, InterruptRequested{now});

  RC_ASSERT(decision.next_state.cooldown == CooldownPhase::kDebouncing);

  // Effects contain exactly one CancelLlm.
  int cancel_count = 0;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<CancelLlm>(effect)) {
      ++cancel_count;
    }
  }
  RC_ASSERT(cancel_count == 1);
}

// ---------------------------------------------------------------------------
// Property 3 (Task 6.4): Debounce buffers without thinking
// For any DialogueState where cooldown == kDebouncing and any
// UserMessageReceived, effects contain RecordMemory and do NOT contain
// StartLlmContinuation.
// **Validates: Requirements 4.2, 4.3**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_debounce_buffers_without_thinking,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kDebouncing;  // Precondition

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
  obs.source = "user";
  auto offset = std::chrono::seconds(*rc::gen::inRange(0, 3600));
  auto now = kBaseTime + offset;
  obs.timestamp = now;

  auto decision = reducer.Reduce(state, UserMessageReceived{obs, now});

  // Effects contain RecordMemory.
  bool has_record_memory = false;
  bool has_start_llm = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<RecordMemory>(effect)) {
      has_record_memory = true;
    }
    if (std::holds_alternative<StartLlmContinuation>(effect)) {
      has_start_llm = true;
    }
  }
  RC_ASSERT(has_record_memory);
  RC_ASSERT(!has_start_llm);
}

// ---------------------------------------------------------------------------
// Property 4 (Task 6.5): Debounce expiry starts thinking
// For any DialogueState where cooldown == kDebouncing and budget not exhausted,
// Reduce(state, DebounceCooldownExpired{now}) produces
// next_state.cooldown == kNone and effects contain StartLlmContinuation.
// **Validates: Requirements 4.4, 13.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_debounce_expiry_starts_thinking,
              (void)) {
  auto config = *genControllerConfig();
  // Use generous config to make budget non-exhausted easier.
  config.max_turns = 50;
  config.token_budget = 100000;
  config.action_count_limit = 100;
  config.turn_timeout = std::chrono::seconds(3600);

  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kDebouncing;  // Precondition

  // Ensure budget is not exhausted: keep counters low.
  state.turn_count = *rc::gen::inRange(0, config.max_turns);
  state.action_count = *rc::gen::inRange(0, config.action_count_limit);
  int max_tokens = config.token_budget - 1;
  state.total_prompt_tokens = *rc::gen::inRange(0, max_tokens);
  state.total_completion_tokens = *rc::gen::inRange(
      0, max_tokens - state.total_prompt_tokens);

  // Use a now that is within the turn_timeout.
  auto elapsed_secs = *rc::gen::inRange(0, 3599);
  auto now = state.session_start + std::chrono::seconds(elapsed_secs);

  // Precondition: budget must not be exhausted.
  RC_PRE(state.turn_count < config.max_turns);
  RC_PRE(state.total_prompt_tokens + state.total_completion_tokens <
         config.token_budget);
  RC_PRE(state.action_count < config.action_count_limit);
  RC_PRE(now - state.session_start < config.turn_timeout);

  auto decision = reducer.Reduce(state, DebounceCooldownExpired{now});

  RC_ASSERT(decision.next_state.cooldown == CooldownPhase::kNone);

  bool has_start_llm = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<StartLlmContinuation>(effect)) {
      has_start_llm = true;
    }
  }
  RC_ASSERT(has_start_llm);
}

// ---------------------------------------------------------------------------
// Property 5 (Task 6.6): Budget exhaustion prevents thinking
// For any DialogueState where budget IS exhausted,
// Reduce(state, DebounceCooldownExpired{now}) produces effects containing
// SignalBudgetExhausted and NOT containing StartLlmContinuation.
// **Validates: Requirements 4.5, 6.1, 6.2, 6.3**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest,
              prop_budget_exhaustion_prevents_thinking, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kDebouncing;

  // Guarantee at least one budget limit is exceeded.
  int exhaustion_method = *rc::gen::inRange(0, 4);
  switch (exhaustion_method) {
    case 0:
      // Exceed turn count.
      state.turn_count = config.max_turns + *rc::gen::inRange(0, 10);
      break;
    case 1:
      // Exceed token budget.
      state.total_prompt_tokens = config.token_budget;
      state.total_completion_tokens = *rc::gen::inRange(0, 1000);
      break;
    case 2:
      // Exceed action count.
      state.action_count = config.action_count_limit + *rc::gen::inRange(0, 10);
      break;
    case 3:
      // Exceed time budget.
      state.session_start = kBaseTime;
      break;
    default:
      break;
  }

  // Compute now: for time-based exhaustion, ensure elapsed >= turn_timeout.
  TimePoint now;
  if (exhaustion_method == 3) {
    auto extra = std::chrono::seconds(*rc::gen::inRange(0, 100));
    now = state.session_start + config.turn_timeout + extra;
  } else {
    // Use a now within timeout so time doesn't accidentally trigger.
    now = state.session_start + std::chrono::seconds(1);
  }

  auto decision = reducer.Reduce(state, DebounceCooldownExpired{now});

  bool has_budget_exhausted = false;
  bool has_start_llm = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<SignalBudgetExhausted>(effect)) {
      has_budget_exhausted = true;
    }
    if (std::holds_alternative<StartLlmContinuation>(effect)) {
      has_start_llm = true;
    }
  }
  RC_ASSERT(has_budget_exhausted);
  RC_ASSERT(!has_start_llm);
}

}  // namespace
}  // namespace shizuru::core::dialogue
