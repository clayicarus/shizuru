// Property-based tests for DefaultDialogueReducer — Phase 1 + Phase 2
// Uses RapidCheck + Google Test
//
// Validates: Requirements 2.6, 2.7, 5.1, 5.3, 6.1, 6.2, 6.3, 7.1, 7.2,
//            7.3, 7.4, 7.5, 7.6, 9.1, 9.2, 9.3, 9.4, 11.1, 12.2,
//            14.1, 14.2, 14.3, 16.4

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <chrono>
#include <algorithm>

#include "controller/config.h"
#include "controller/types.h"
#include "conversation/item.h"
#include "dialogue/default_reducer.h"
#include "dialogue/types.h"

namespace shizuru::core::dialogue {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using namespace std::chrono_literals;

// Fixed base time for deterministic generation.
const TimePoint kBaseTime = Clock::now();

// ---------------------------------------------------------------------------
// Helper: check if any effect in the list holds the given alternative.
// ---------------------------------------------------------------------------
template <typename T>
bool HasEffect(const std::vector<DialogueEffect>& effects) {
  for (const auto& e : effects) {
    if (std::holds_alternative<T>(e)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// RapidCheck generators
// ---------------------------------------------------------------------------

rc::Gen<Observation> genObservation(ObservationType type) {
  return rc::gen::exec([type] {
    Observation obs;
    obs.type = type;
    auto content = *rc::gen::nonEmpty(
        rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
    auto source = *rc::gen::element(
        std::string("user"), std::string("tool:web_search"),
        std::string("scheduler"), std::string("system"));
    obs.timestamp = kBaseTime +
        std::chrono::seconds(*rc::gen::inRange(0, 3600));
    switch (type) {
      case ObservationType::kUserMessage:
        obs.item = conversation::MakeHumanMessageItem(source, "", content);
        break;
      case ObservationType::kToolResult:
        obs.item = conversation::MakeToolResultItem(source, source, conversation::ParseJsonOrString(content));
        break;
      case ObservationType::kSystemEvent:
        obs.item = conversation::MakeSystemEventItem("system:" + source, "", "event", source, conversation::ParseJsonOrString(content));
        break;
      default:
        obs.item = conversation::MakeHumanMessageItem(source, "", content);
        break;
    }
    return obs;
  });
}

rc::Gen<ToolCall> genToolCall() {
  return rc::gen::exec([] {
    ToolCall tc;
    tc.id = "call_" + std::to_string(*rc::gen::inRange(1, 10000));
    tc.name = *rc::gen::element(
        std::string("web_search"), std::string("calculator"),
        std::string("file_read"), std::string("code_exec"));
    tc.arguments = R"({"arg":"val"})";
    tc.required_capability = "";
    return tc;
  });
}

rc::Gen<ActionCandidate> genActionCandidate() {
  return rc::gen::exec([] {
    ActionCandidate c;
    int choice = *rc::gen::inRange(0, 3);
    switch (choice) {
      case 0: {
        c.type = ActionType::kToolCall;
        int n = *rc::gen::inRange(1, 4);
        for (int i = 0; i < n; ++i) {
          c.tool_calls.push_back(*genToolCall());
        }
        break;
      }
      case 1:
        c.type = ActionType::kResponse;
        c.response_text = "response_text";
        break;
      case 2:
      default:
        c.type = ActionType::kContinue;
        break;
    }
    return c;
  });
}

rc::Gen<DialogueState> genDialogueState() {
  return rc::gen::exec([] {
    DialogueState s;
    s.conversation_active = *rc::gen::arbitrary<bool>();
    s.cooldown = *rc::gen::element(CooldownPhase::kNone,
                                   CooldownPhase::kDebouncing);
    s.turn_llm_calls = *rc::gen::inRange(0, 50);
    s.turn_prompt_tokens = *rc::gen::inRange(0, 50000);
    s.turn_completion_tokens = *rc::gen::inRange(0, 50000);
    s.turn_action_count = *rc::gen::inRange(0, 100);
    s.turn_continuation_count = *rc::gen::inRange(0, 20);
    s.session_start = kBaseTime;
    s.last_activity =
        kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

    // Phase 2 fields
    s.deliberation = *rc::gen::element(
        DeliberationPhase::kIdle,
        DeliberationPhase::kAwaitingTurnTrigger,
        DeliberationPhase::kThinking,
        DeliberationPhase::kAwaitingToolResults);

    s.next_turn_trigger_id = *rc::gen::inRange<uint64_t>(0, 1000);

    // Maintain invariant: pending_turn_trigger_id <= next_turn_trigger_id
    if (s.deliberation == DeliberationPhase::kAwaitingTurnTrigger) {
      // Must have pending_turn_trigger_id > 0
      s.pending_turn_trigger_id =
          *rc::gen::inRange<uint64_t>(1, s.next_turn_trigger_id + 1);
      if (s.pending_turn_trigger_id > s.next_turn_trigger_id) {
        s.next_turn_trigger_id = s.pending_turn_trigger_id;
      }
    } else {
      s.pending_turn_trigger_id = 0;
    }

    // Maintain invariant: pending_tool_call_ids non-empty iff kAwaitingToolResults
    if (s.deliberation == DeliberationPhase::kAwaitingToolResults) {
      int n = *rc::gen::inRange(1, 4);
      for (int i = 0; i < n; ++i) {
        s.pending_tool_call_ids.push_back(
            "call_" + std::to_string(*rc::gen::inRange(1, 10000)));
      }
    } else {
      s.pending_tool_call_ids.clear();
      s.pending_tool_results.clear();
    }

    return s;
  });
}

rc::Gen<DialogueEvent> genDialogueEvent() {
  return rc::gen::exec([] {
    int choice = *rc::gen::inRange(0, 12);
    auto offset = std::chrono::seconds(*rc::gen::inRange(0, 3600));
    auto now = kBaseTime + offset;

    switch (choice) {
      case 0:
        return DialogueEvent{InterruptRequested{now}};
      case 1:
        return DialogueEvent{DebounceCooldownExpired{now}};
      case 2: {
        auto obs = *genObservation(ObservationType::kUserMessage);
        return DialogueEvent{UserMessageReceived{obs, now}};
      }
      case 3:
        return DialogueEvent{ShutdownRequested{}};
      case 4: {
        auto candidate = *genActionCandidate();
        int pt = *rc::gen::inRange(0, 5000);
        int ct = *rc::gen::inRange(0, 5000);
        return DialogueEvent{LlmCompleted{candidate, pt, ct, now}};
      }
      case 5: {
        std::string reason = *rc::gen::element(
            std::string("timeout"), std::string("rate_limit"),
            std::string("server_error"));
        return DialogueEvent{LlmFailed{reason, now}};
      }
      case 6: {
        auto obs = *genObservation(ObservationType::kToolResult);
        return DialogueEvent{ToolResultReceived{obs, now}};
      }
      case 7: {
        int n = *rc::gen::inRange(1, 4);
        std::vector<std::string> ids;
        for (int i = 0; i < n; ++i) {
          ids.push_back("call_" + std::to_string(*rc::gen::inRange(1, 10000)));
        }
        return DialogueEvent{ToolCallTimeout{ids, now}};
      }
      case 8: {
        std::string source = *rc::gen::element(
            std::string("tool_results_complete"),
            std::string("continuation"));
        return DialogueEvent{ContinuationRequested{source, now}};
      }
      case 9: {
        auto obs = *genObservation(ObservationType::kSystemEvent);
        return DialogueEvent{SystemEventReceived{obs, now}};
      }
      case 10: {
        auto kind = *rc::gen::element(
            TimerKind::kDebounce, TimerKind::kToolCallTimeout,
            TimerKind::kConversationIdle);
        std::string timer_id = *rc::gen::element(
            std::string("debounce"), std::string("tool_call_timeout"),
            std::string("idle"));
        return DialogueEvent{TimerExpired{kind, timer_id, now}};
      }
      case 11:
      default: {
        uint64_t obs_id = *rc::gen::inRange<uint64_t>(0, 100);
        auto verdict = *rc::gen::element(
            TurnTriggerVerdict::kRespondNow,
            TurnTriggerVerdict::kStoreOnly);
        return DialogueEvent{TurnTriggerClassified{obs_id, verdict, now}};
      }
    }
  });
}

rc::Gen<ControllerConfig> genControllerConfig() {
  return rc::gen::exec([] {
    ControllerConfig cfg;
    cfg.token_budget = *rc::gen::inRange(1000, 100000);
    cfg.action_count_limit = *rc::gen::inRange(1, 100);
    cfg.max_continuations = *rc::gen::inRange(1, 20);
    return cfg;
  });
}

// ---------------------------------------------------------------------------
// Property 1 (Task 7.1): Reducer purity — extended for Phase 2
// For any valid DialogueState (including Phase 2 fields) and DialogueEvent
// (including Phase 2 events), calling Reduce twice with identical inputs
// produces identical outputs.
// **Validates: Requirements 16.4**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_reducer_purity, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto event = *genDialogueEvent();

  auto decision1 = reducer.Reduce(state, event);
  auto decision2 = reducer.Reduce(state, event);

  // Phase 1 state fields
  RC_ASSERT(decision1.next_state.conversation_active ==
            decision2.next_state.conversation_active);
  RC_ASSERT(decision1.next_state.cooldown == decision2.next_state.cooldown);
  RC_ASSERT(decision1.next_state.turn_llm_calls == decision2.next_state.turn_llm_calls);
  RC_ASSERT(decision1.next_state.turn_prompt_tokens ==
            decision2.next_state.turn_prompt_tokens);
  RC_ASSERT(decision1.next_state.turn_completion_tokens ==
            decision2.next_state.turn_completion_tokens);
  RC_ASSERT(decision1.next_state.turn_action_count ==
            decision2.next_state.turn_action_count);
  RC_ASSERT(decision1.next_state.session_start ==
            decision2.next_state.session_start);
  RC_ASSERT(decision1.next_state.last_activity ==
            decision2.next_state.last_activity);

  // Phase 2 state fields
  RC_ASSERT(decision1.next_state.deliberation ==
            decision2.next_state.deliberation);
  RC_ASSERT(decision1.next_state.next_turn_trigger_id ==
            decision2.next_state.next_turn_trigger_id);
  RC_ASSERT(decision1.next_state.pending_turn_trigger_id ==
            decision2.next_state.pending_turn_trigger_id);
  RC_ASSERT(decision1.next_state.pending_tool_call_ids ==
            decision2.next_state.pending_tool_call_ids);
  RC_ASSERT(decision1.next_state.pending_tool_results ==
            decision2.next_state.pending_tool_results);

  // Effects must match in count and variant indices.
  RC_ASSERT(decision1.effects.size() == decision2.effects.size());
  for (size_t i = 0; i < decision1.effects.size(); ++i) {
    RC_ASSERT(decision1.effects[i].index() == decision2.effects[i].index());
  }
}

// ---------------------------------------------------------------------------
// Property 2 (Task 7.2): State consistency invariants
// For any state and event, if next_state.deliberation == kAwaitingTurnTrigger
// then pending_turn_trigger_id > 0, and if kAwaitingToolResults then
// pending_tool_call_ids is non-empty.
// **Validates: Requirements 2.6, 2.7**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_state_consistency_invariants,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto event = *genDialogueEvent();

  auto decision = reducer.Reduce(state, event);
  const auto& ns = decision.next_state;

  if (ns.deliberation == DeliberationPhase::kAwaitingTurnTrigger) {
    RC_ASSERT(ns.pending_turn_trigger_id > 0);
  }
  if (ns.deliberation == DeliberationPhase::kAwaitingToolResults) {
    RC_ASSERT(!ns.pending_tool_call_ids.empty());
  }
}

// ---------------------------------------------------------------------------
// Property 5 (Task 7.5): Normal user message records memory and triggers
// turn evaluation.
// Precondition: cooldown == kNone, deliberation == kIdle
// **Validates: Requirements 5.1**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest,
              prop_user_message_records_memory_and_triggers_turn, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kNone;
  state.deliberation = DeliberationPhase::kIdle;
  state.pending_turn_trigger_id = 0;
  state.pending_tool_call_ids.clear();
  state.pending_tool_results.clear();

  auto obs = *genObservation(ObservationType::kUserMessage);
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  auto decision = reducer.Reduce(state, UserMessageReceived{obs, now});

  RC_ASSERT(HasEffect<BufferToWorkspace>(decision.effects));
  RC_ASSERT(HasEffect<CommitWorkspace>(decision.effects));
  RC_ASSERT(HasEffect<StartTurnTriggerClassification>(decision.effects));
  RC_ASSERT(decision.next_state.deliberation ==
            DeliberationPhase::kAwaitingTurnTrigger);
}

// ---------------------------------------------------------------------------
// Property 7 (Task 7.6): Turn-trigger verdict routing
// Precondition: deliberation == kAwaitingTurnTrigger, matching obs_id
// kRespondNow → kThinking + StartLlm; kStoreOnly → kIdle + empty effects
// **Validates: Requirements 6.1, 6.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_turn_trigger_verdict_routing,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  uint64_t id = *rc::gen::inRange<uint64_t>(1, 1000);
  state.pending_turn_trigger_id = id;
  state.next_turn_trigger_id = id;
  state.pending_tool_call_ids.clear();
  state.pending_tool_results.clear();

  auto verdict = *rc::gen::element(
      TurnTriggerVerdict::kRespondNow, TurnTriggerVerdict::kStoreOnly);
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  auto decision = reducer.Reduce(
      state, TurnTriggerClassified{id, verdict, now});

  if (verdict == TurnTriggerVerdict::kRespondNow) {
    RC_ASSERT(decision.next_state.deliberation ==
              DeliberationPhase::kThinking);
    RC_ASSERT(HasEffect<StartLlm>(decision.effects));
  } else {
    RC_ASSERT(decision.next_state.deliberation ==
              DeliberationPhase::kIdle);
    // kStoreOnly emits EmitActivityEffect{kInputStored} — not empty.
    RC_ASSERT(!HasEffect<StartLlm>(decision.effects));
    RC_ASSERT(HasEffect<EmitActivityEffect>(decision.effects));
  }
}

// ---------------------------------------------------------------------------
// Property 8 (Task 7.7): Stale turn-trigger rejection
// Non-matching obs_id → state unchanged, no effects
// **Validates: Requirements 6.3**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_stale_turn_trigger_rejection,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  // Generate an obs_id that does NOT match pending_turn_trigger_id.
  uint64_t stale_id = *rc::gen::inRange<uint64_t>(0, 1000);
  RC_PRE(stale_id != state.pending_turn_trigger_id);

  auto verdict = *rc::gen::element(
      TurnTriggerVerdict::kRespondNow, TurnTriggerVerdict::kStoreOnly);

  auto decision = reducer.Reduce(
      state, TurnTriggerClassified{stale_id, verdict, now});

  // State unchanged.
  RC_ASSERT(decision.next_state.deliberation == state.deliberation);
  RC_ASSERT(decision.next_state.pending_turn_trigger_id ==
            state.pending_turn_trigger_id);
  RC_ASSERT(decision.next_state.cooldown == state.cooldown);
  RC_ASSERT(decision.next_state.turn_llm_calls == state.turn_llm_calls);
  // No effects.
  RC_ASSERT(decision.effects.empty());
}

// ---------------------------------------------------------------------------
// Property 9 (Task 7.8): LLM completion token and turn accounting
// turn_count == state.turn_llm_calls + 1, tokens accumulated correctly
// **Validates: Requirements 7.1, 7.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_llm_completion_accounting,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto candidate = *genActionCandidate();
  int pt = *rc::gen::inRange(0, 5000);
  int ct = *rc::gen::inRange(0, 5000);
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  auto decision = reducer.Reduce(
      state, LlmCompleted{candidate, pt, ct, now});

  RC_ASSERT(decision.next_state.turn_llm_calls == state.turn_llm_calls + 1);
  RC_ASSERT(decision.next_state.turn_prompt_tokens ==
            state.turn_prompt_tokens + pt);
  RC_ASSERT(decision.next_state.turn_completion_tokens ==
            state.turn_completion_tokens + ct);
}

// ---------------------------------------------------------------------------
// Property 10 (Task 7.9): LLM result routing by action type
// kToolCall → EmitToolCallFrames + kAwaitingToolResults
// kResponse → DeliverResponse + kIdle
// kContinue → StartLlm + kThinking
// **Validates: Requirements 7.3, 7.4, 7.5**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_llm_result_routing, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto candidate = *genActionCandidate();
  int pt = *rc::gen::inRange(0, 5000);
  int ct = *rc::gen::inRange(0, 5000);
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  auto decision = reducer.Reduce(
      state, LlmCompleted{candidate, pt, ct, now});

  switch (candidate.type) {
    case ActionType::kToolCall:
      RC_ASSERT(HasEffect<EmitToolCallFrames>(decision.effects));
      RC_ASSERT(decision.next_state.deliberation ==
                DeliberationPhase::kAwaitingToolResults);
      RC_ASSERT(!decision.next_state.pending_tool_call_ids.empty());
      break;
    case ActionType::kResponse:
      RC_ASSERT(HasEffect<DeliverResponse>(decision.effects));
      RC_ASSERT(decision.next_state.deliberation ==
                DeliberationPhase::kIdle);
      break;
    case ActionType::kContinue:
      // If continuation limit not reached: StartLlm + kThinking.
      // If continuation limit reached: SignalBudgetExhausted + kIdle.
      if (decision.next_state.turn_continuation_count < config.max_continuations) {
        RC_ASSERT(HasEffect<StartLlm>(decision.effects));
        RC_ASSERT(decision.next_state.deliberation ==
                  DeliberationPhase::kThinking);
      } else {
        RC_ASSERT(HasEffect<SignalBudgetExhausted>(decision.effects));
        RC_ASSERT(decision.next_state.deliberation ==
                  DeliberationPhase::kIdle);
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Property 12 (Task 7.10): Tool result collection and continuation
// Precondition: deliberation == kAwaitingToolResults
// Result recorded; if all in → kThinking + StartLlm; if partial → stays
// kAwaitingToolResults
// **Validates: Requirements 9.1, 9.2, 9.3**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_tool_result_collection, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.deliberation = DeliberationPhase::kAwaitingToolResults;

  // Generate 1-3 pending tool call ids.
  int n = *rc::gen::inRange(1, 4);
  state.pending_tool_call_ids.clear();
  state.pending_tool_results.clear();
  for (int i = 0; i < n; ++i) {
    state.pending_tool_call_ids.push_back("call_" + std::to_string(i));
  }

  // Pick one of the pending ids to receive a result for.
  int idx = *rc::gen::inRange(0, n);
  std::string result_id = state.pending_tool_call_ids[idx];

  Observation obs;
  obs.type = ObservationType::kToolResult;
  obs.item = conversation::MakeToolResultItem(result_id, result_id, conversation::ParseJsonOrString(R"({"result":"ok"})"));
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));
  obs.timestamp = now;

  auto decision = reducer.Reduce(state, ToolResultReceived{obs, now});

  RC_ASSERT(HasEffect<RecordToolResult>(decision.effects));

  // Check if all results are now in. Since we started with no prior results
  // and submitted one, all_complete is true iff n == 1.
  bool all_complete = (n == 1);

  if (all_complete) {
    // When all results are in, the reducer transitions to kThinking and
    // clears pending collections.
    RC_ASSERT(decision.next_state.deliberation ==
              DeliberationPhase::kThinking);
    RC_ASSERT(decision.next_state.pending_tool_call_ids.empty());
    RC_ASSERT(decision.next_state.pending_tool_results.empty());
    RC_ASSERT(HasEffect<StartLlm>(decision.effects));
    // Bug fix: CancelTimer must be emitted to prevent stale timeout.
    RC_ASSERT(HasEffect<CancelTimer>(decision.effects));
  } else {
    // Partial: result recorded, stays awaiting.
    RC_ASSERT(decision.next_state.pending_tool_results.count(result_id) == 1);
    RC_ASSERT(decision.next_state.deliberation ==
              DeliberationPhase::kAwaitingToolResults);
  }
}

// ---------------------------------------------------------------------------
// Property 14 (Task 7.11): Interrupt cancels in-flight tool work
// Precondition: deliberation is kThinking or kAwaitingToolResults
// Effects contain ScheduleTimer{kDebounce} and RecordInterruptMemory;
// if kAwaitingToolResults → CancelTimer
// **Validates: Requirements 11.1, 14.1, 14.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_interrupt_cancels_inflight_work,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kNone;

  // Precondition: interruptible deliberation states only.
  auto delib = *rc::gen::element(
      DeliberationPhase::kThinking,
      DeliberationPhase::kAwaitingToolResults);
  state.deliberation = delib;

  if (delib == DeliberationPhase::kAwaitingToolResults) {
    if (state.pending_tool_call_ids.empty()) {
      state.pending_tool_call_ids.push_back("call_1");
    }
  }

  RC_PRE(state.deliberation == DeliberationPhase::kThinking ||
         state.deliberation == DeliberationPhase::kAwaitingToolResults);

  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));
  auto decision = reducer.Reduce(state, InterruptRequested{now});

  RC_ASSERT(HasEffect<ScheduleTimer>(decision.effects));
  RC_ASSERT(HasEffect<RecordInterruptMemory>(decision.effects));

  // Verify ScheduleTimer is kDebounce.
  for (const auto& e : decision.effects) {
    if (std::holds_alternative<ScheduleTimer>(e)) {
      RC_ASSERT(std::get<ScheduleTimer>(e).kind == TimerKind::kDebounce);
    }
  }

  if (state.deliberation == DeliberationPhase::kAwaitingToolResults) {
    RC_ASSERT(HasEffect<CancelTimer>(decision.effects));
  }
}

// ---------------------------------------------------------------------------
// Property 16 (Task 7.12): Activity tracking on message events
// For UserMessageReceived, SystemEventReceived, ToolResultReceived,
// LlmCompleted: conversation_active == true and last_activity == event.now
// **Validates: Requirements 5.3, 7.6, 9.4, 12.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_activity_tracking, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  int event_type = *rc::gen::inRange(0, 4);
  DialogueEvent event;

  switch (event_type) {
    case 0: {
      // UserMessageReceived — only tracks activity when cooldown == kNone
      // and deliberation is kIdle or kAwaitingTurnTrigger.
      state.cooldown = CooldownPhase::kNone;
      state.deliberation = *rc::gen::element(
          DeliberationPhase::kIdle,
          DeliberationPhase::kAwaitingTurnTrigger);
      if (state.deliberation == DeliberationPhase::kAwaitingTurnTrigger) {
        if (state.pending_turn_trigger_id == 0) {
          state.pending_turn_trigger_id = 1;
          state.next_turn_trigger_id = 1;
        }
      }
      auto obs = *genObservation(ObservationType::kUserMessage);
      event = UserMessageReceived{obs, now};
      break;
    }
    case 1: {
      // SystemEventReceived — only tracks when deliberation == kIdle.
      state.deliberation = DeliberationPhase::kIdle;
      auto obs = *genObservation(ObservationType::kSystemEvent);
      event = SystemEventReceived{obs, now};
      break;
    }
    case 2: {
      // ToolResultReceived — tracks activity always.
      state.deliberation = DeliberationPhase::kAwaitingToolResults;
      if (state.pending_tool_call_ids.empty()) {
        state.pending_tool_call_ids.push_back("call_1");
      }
      Observation obs;
      obs.type = ObservationType::kToolResult;
      obs.item = conversation::MakeToolResultItem(state.pending_tool_call_ids[0], state.pending_tool_call_ids[0], conversation::ParseJsonOrString(R"({"r":"ok"})"));
      obs.timestamp = now;
      event = ToolResultReceived{obs, now};
      break;
    }
    case 3:
    default: {
      // LlmCompleted — always tracks activity.
      auto candidate = *genActionCandidate();
      int pt = *rc::gen::inRange(0, 5000);
      int ct = *rc::gen::inRange(0, 5000);
      event = LlmCompleted{candidate, pt, ct, now};
      break;
    }
  }

  auto decision = reducer.Reduce(state, event);

  RC_ASSERT(decision.next_state.conversation_active == true);
  RC_ASSERT(decision.next_state.last_activity == now);
}

// ---------------------------------------------------------------------------
// Property 17 (Task 7.13): kStoreOnly does not start an assistant turn
// Precondition: deliberation == kAwaitingTurnTrigger, matching obs_id,
// verdict == kStoreOnly
// Effects SHALL NOT contain StartLlm
// **Validates: Requirements 6.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest, prop_store_only_no_start_llm,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  uint64_t id = *rc::gen::inRange<uint64_t>(1, 1000);
  state.pending_turn_trigger_id = id;
  state.next_turn_trigger_id = id;
  state.pending_tool_call_ids.clear();
  state.pending_tool_results.clear();

  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  auto decision = reducer.Reduce(
      state, TurnTriggerClassified{id, TurnTriggerVerdict::kStoreOnly, now});

  RC_ASSERT(!HasEffect<StartLlm>(decision.effects));
}

// ---------------------------------------------------------------------------
// Property 19 (Task 7.15): Interrupt always records interrupt memory
// Precondition: deliberation is kThinking or kAwaitingToolResults
// Effects SHALL contain RecordInterruptMemory
// **Validates: Requirements 14.2**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest,
              prop_interrupt_always_records_interrupt_memory, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kNone;

  auto delib = *rc::gen::element(
      DeliberationPhase::kThinking,
      DeliberationPhase::kAwaitingToolResults);
  state.deliberation = delib;

  if (delib == DeliberationPhase::kAwaitingToolResults) {
    if (state.pending_tool_call_ids.empty()) {
      state.pending_tool_call_ids.push_back("call_1");
    }
  }

  RC_PRE(state.deliberation == DeliberationPhase::kThinking ||
         state.deliberation == DeliberationPhase::kAwaitingToolResults);

  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));
  auto decision = reducer.Reduce(state, InterruptRequested{now});

  RC_ASSERT(HasEffect<RecordInterruptMemory>(decision.effects));
}

// ---------------------------------------------------------------------------
// Property 20 (Task 7.16): Superseding UserMessageReceived cancels pending
// turn-trigger
// Precondition: deliberation == kAwaitingTurnTrigger
// Effects contain CancelTurnTriggerClassification and
// StartTurnTriggerClassification, pending_turn_trigger_id changed
// **Validates: Requirements 14.3**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(DialogueReducerPropTest,
              prop_superseding_message_cancels_turn_trigger, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kNone;
  state.deliberation = DeliberationPhase::kAwaitingTurnTrigger;
  uint64_t old_id = *rc::gen::inRange<uint64_t>(1, 1000);
  state.pending_turn_trigger_id = old_id;
  state.next_turn_trigger_id = old_id;
  state.pending_tool_call_ids.clear();
  state.pending_tool_results.clear();

  auto obs = *genObservation(ObservationType::kUserMessage);
  auto now = kBaseTime + std::chrono::seconds(*rc::gen::inRange(0, 3600));

  auto decision = reducer.Reduce(state, UserMessageReceived{obs, now});

  RC_ASSERT(HasEffect<CancelTurnTriggerClassification>(decision.effects));
  RC_ASSERT(HasEffect<StartTurnTriggerClassification>(decision.effects));
  RC_ASSERT(decision.next_state.pending_turn_trigger_id !=
            state.pending_turn_trigger_id);
}

// ---------------------------------------------------------------------------
// Phase 1 properties (preserved)
// ---------------------------------------------------------------------------

// Property: Barge-in enters debounce
RC_GTEST_PROP(DialogueReducerPropTest, prop_bargein_enters_debounce, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kNone;

  auto offset = std::chrono::seconds(*rc::gen::inRange(0, 3600));
  auto now = kBaseTime + offset;

  auto decision = reducer.Reduce(state, InterruptRequested{now});

  RC_ASSERT(decision.next_state.cooldown == CooldownPhase::kDebouncing);

  int cancel_count = 0;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<CancelLlm>(effect)) {
      ++cancel_count;
    }
  }
  RC_ASSERT(cancel_count == 1);
}

// Property: Debounce buffers without thinking
RC_GTEST_PROP(DialogueReducerPropTest, prop_debounce_buffers_without_thinking,
              (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kDebouncing;

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  auto content = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
  obs.item = conversation::MakeHumanMessageItem("user", "", content);
  auto offset = std::chrono::seconds(*rc::gen::inRange(0, 3600));
  auto now = kBaseTime + offset;
  obs.timestamp = now;

  auto decision = reducer.Reduce(state, UserMessageReceived{obs, now});

  bool has_buffer_to_workspace = false;
  bool has_start_llm = false;
  for (const auto& effect : decision.effects) {
    if (std::holds_alternative<BufferToWorkspace>(effect)) {
      has_buffer_to_workspace = true;
    }
    if (std::holds_alternative<StartLlmContinuation>(effect)) {
      has_start_llm = true;
    }
  }
  RC_ASSERT(has_buffer_to_workspace);
  RC_ASSERT(!has_start_llm);
}

// Property: Debounce expiry starts thinking
RC_GTEST_PROP(DialogueReducerPropTest, prop_debounce_expiry_starts_thinking,
              (void)) {
  auto config = *genControllerConfig();
  config.action_count_limit = 50;
  config.token_budget = 100000;
  config.action_count_limit = 100;
  config.max_continuations = 100;

  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kDebouncing;

  state.turn_llm_calls = *rc::gen::inRange(0, config.action_count_limit);
  state.turn_action_count = *rc::gen::inRange(0, config.action_count_limit);
  int max_tokens = config.token_budget - 1;
  state.turn_prompt_tokens = *rc::gen::inRange(0, max_tokens);
  state.turn_completion_tokens = *rc::gen::inRange(
      0, max_tokens - state.turn_prompt_tokens);

  auto elapsed_secs = *rc::gen::inRange(0, 3599);
  auto now = state.session_start + std::chrono::seconds(elapsed_secs);

  RC_PRE(state.turn_prompt_tokens + state.turn_completion_tokens <
         config.token_budget);
  RC_PRE(state.turn_action_count < config.action_count_limit);
  RC_PRE(state.turn_continuation_count < config.max_continuations);

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

// Property: Budget exhaustion prevents thinking
RC_GTEST_PROP(DialogueReducerPropTest,
              prop_budget_exhaustion_prevents_thinking, (void)) {
  auto config = *genControllerConfig();
  DefaultDialogueReducer reducer(config);

  auto state = *genDialogueState();
  state.cooldown = CooldownPhase::kDebouncing;

  // Exhaust one of the three per-turn limits.
  int exhaustion_method = *rc::gen::inRange(0, 3);
  switch (exhaustion_method) {
    case 0:
      // Token budget exhausted.
      state.turn_prompt_tokens = config.token_budget;
      state.turn_completion_tokens = *rc::gen::inRange(0, 1000);
      break;
    case 1:
      // Action count limit exhausted.
      state.turn_action_count = config.action_count_limit + *rc::gen::inRange(0, 10);
      break;
    case 2:
      // Continuation limit exhausted.
      state.turn_continuation_count = config.max_continuations + *rc::gen::inRange(0, 10);
      break;
    default:
      break;
  }

  auto now = state.session_start + std::chrono::seconds(1);

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
