#include "dialogue/default_reducer.h"

namespace shizuru::core::dialogue {

namespace {

template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

DefaultDialogueReducer::DefaultDialogueReducer(const ControllerConfig& config)
    : config_(config) {}

// ---------------------------------------------------------------------------
// Reduce — dispatch via std::visit
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::Reduce(
    const DialogueState& state,
    const DialogueEvent& event) const {
  return std::visit(overloaded{
    [&](const InterruptRequested& e) {
      return HandleInterrupt(state, e.now);
    },
    [&](const DebounceCooldownExpired& e) {
      return HandleDebounceCooldownExpired(state, e.now);
    },
    [&](const ShutdownRequested&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const LlmCompleted& e) {
      return HandleLlmCompleted(state, e);
    },
    [&](const LlmFailed& e) {
      return HandleLlmFailed(state, e);
    },
    [&](const ToolCallTimeout& e) {
      return HandleToolCallTimeout(state, e);
    },
    [&](const ContinuationRequested& e) {
      return HandleContinuation(state, e);
    },
    [&](const TimerExpired& e) {
      return HandleTimerExpired(state, e);
    },
    [&](const TurnTriggerClassified& e) {
      return HandleTurnTriggerClassified(state, e);
    },
    [&](const ConversationItemReceived& e) {
      return HandleConversationItemReceived(state, e);
    },
    [&](const ToolResultReceived& e) {
      return HandleToolResult(state, e);
    },
    [&](const SystemEventReceived& e) {
      return HandleSystemEvent(state, e);
    },
  }, event);
}

// ---------------------------------------------------------------------------
// HandleInterrupt
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleInterrupt(
    const DialogueState& state,
    std::chrono::steady_clock::time_point now) const {
  auto next = state;
  next.cooldown = CooldownPhase::kDebouncing;
  next.last_activity = now;
  next.deliberation = DeliberationPhase::kIdle;
  next.pending_turn_trigger_id = 0;
  next.pending_tool_call_ids.clear();
  next.pending_tool_results.clear();

  std::vector<DialogueEffect> effects;
  effects.push_back(CancelLlm{});
  effects.push_back(RecordInterruptMemory{});
  effects.push_back(ScheduleTimer{
      TimerKind::kDebounce,
      "debounce",
      now + config_.debounce_duration});

  if (state.deliberation == DeliberationPhase::kAwaitingToolResults) {
    effects.push_back(CancelTimer{"tool_call_timeout"});
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleDebounceCooldownExpired
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleDebounceCooldownExpired(
    const DialogueState& state,
    std::chrono::steady_clock::time_point now) const {
  auto next = state;
  next.cooldown = CooldownPhase::kNone;

  std::vector<DialogueEffect> effects;

  if (IsBudgetExhausted(next, now)) {
    effects.push_back(SignalBudgetExhausted{});
  } else {
    effects.push_back(StartLlmContinuation{now});
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleConversationItemReceived
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleConversationItemReceived(
    const DialogueState& state,
    const ConversationItemReceived& event) const {
  // During debounce, record the item and stay in debounce.
  if (state.cooldown == CooldownPhase::kDebouncing) {
    auto next = state;
    next.last_activity = event.now;
    next.conversation_active = true;

    std::vector<DialogueEffect> effects;
    effects.push_back(RecordConversationItem{event.item});
    return {next, effects};
  }

  // Turn-trigger filtering is currently disabled. Meaningful conversation items
  // should respond immediately instead of entering kAwaitingTurnTrigger.
  if (state.deliberation == DeliberationPhase::kIdle ||
      state.deliberation == DeliberationPhase::kAwaitingTurnTrigger) {
    auto next = state;
    next.conversation_active = true;
    next.last_activity = event.now;

    std::vector<DialogueEffect> effects;

    // Reset per-turn counters.
    next.turn_llm_calls = 0;
    next.turn_prompt_tokens = 0;
    next.turn_completion_tokens = 0;
    next.turn_action_count = 0;
    next.turn_continuation_count = 0;

    next.pending_turn_trigger_id = 0;
    next.deliberation = DeliberationPhase::kThinking;

    effects.push_back(StartLlmWithBatch{{event.item}});

    return {next, effects};
  }

  // Other deliberation states: no-op.
  return {state, {}};
}

// ---------------------------------------------------------------------------
// HandleTurnTriggerClassified
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleTurnTriggerClassified(
    const DialogueState& state,
    const TurnTriggerClassified& event) const {
  // Stale rejection.
  if (event.obs_id != state.pending_turn_trigger_id) {
    return {state, {}};
  }

  auto next = state;
  next.pending_turn_trigger_id = 0;

  if (event.verdict == TurnTriggerVerdict::kRespondNow) {
    next.deliberation = DeliberationPhase::kThinking;
    return {next, {StartLlmContinuation{event.now}}};
  }

  // kStoreOnly: message stays in committed history, no assistant turn.
  next.deliberation = DeliberationPhase::kIdle;
  return {next, {EmitActivityEffect{ActivityKind::kInputStored, ""}}};
}

// ---------------------------------------------------------------------------
// HandleLlmCompleted
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleLlmCompleted(
    const DialogueState& state,
    const LlmCompleted& event) const {
  auto next = state;

  next.turn_llm_calls = state.turn_llm_calls + 1;
  next.turn_prompt_tokens = state.turn_prompt_tokens + event.prompt_tokens;
  next.turn_completion_tokens =
      state.turn_completion_tokens + event.completion_tokens;
  next.last_activity = event.now;
  next.conversation_active = true;

  std::vector<DialogueEffect> effects;

  switch (event.candidate.type) {
    case ActionType::kToolCall: {
      next.deliberation = DeliberationPhase::kAwaitingToolResults;
      next.pending_tool_call_ids.clear();
      next.pending_tool_results.clear();
      for (const auto& tc : event.candidate.tool_calls) {
        next.pending_tool_call_ids.push_back(tc.id);
      }
      effects.push_back(RecordToolCallDecision{event.candidate});
      effects.push_back(EmitToolCallFrames{event.candidate});
      effects.push_back(ScheduleTimer{
          TimerKind::kToolCallTimeout,
          "tool_call_timeout",
          event.now + config_.tool_call_timeout});
      break;
    }
    case ActionType::kResponse: {
      next.deliberation = DeliberationPhase::kIdle;
      effects.push_back(DeliverResponse{event.candidate});
      break;
    }
    case ActionType::kContinue: {
      next.turn_continuation_count = state.turn_continuation_count + 1;
      if (next.turn_continuation_count >= config_.max_continuations) {
        next.deliberation = DeliberationPhase::kIdle;
        effects.push_back(SignalBudgetExhausted{});
      } else {
        next.deliberation = DeliberationPhase::kThinking;
        effects.push_back(StartLlmContinuation{event.now});
      }
      break;
    }
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleLlmFailed
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleLlmFailed(
    const DialogueState& state,
    const LlmFailed& event) const {
  auto next = state;
  next.deliberation = DeliberationPhase::kIdle;

  std::vector<DialogueEffect> effects;
  effects.push_back(EmitDiagnosticEffect{event.reason});
  effects.push_back(TransitionState{Event::kLlmFailure});

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleToolResult
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleToolResult(
    const DialogueState& state,
    const ToolResultReceived& event) const {
  auto next = state;
  next.last_activity = event.now;
  next.conversation_active = true;

  // Record the result. Pair by tool_call_id when available.
  std::string result_key;
  for (const auto& part : event.item.parts) {
    if (const auto* trp = std::get_if<ToolResultPart>(&part)) {
      result_key = trp->tool_call_id;
      if (!result_key.empty()) {
        break;
      }
    }
  }
  if (result_key.empty()) {
    result_key = event.item.item_id;
  }
  if (result_key.empty()) {
    result_key = event.item.actor.actor_id;
  }
  next.pending_tool_results[result_key] = "received";

  std::vector<DialogueEffect> effects;
  effects.push_back(RecordToolResultItem{event.item});

  // Check if all pending tool calls have results.
  bool all_complete = true;
  for (const auto& id : next.pending_tool_call_ids) {
    if (next.pending_tool_results.find(id) ==
        next.pending_tool_results.end()) {
      all_complete = false;
      break;
    }
  }

  if (all_complete) {
    next.deliberation = DeliberationPhase::kThinking;
    next.pending_tool_call_ids.clear();
    next.pending_tool_results.clear();
    effects.push_back(CancelTimer{"tool_call_timeout"});
    effects.push_back(StartLlmContinuation{event.now});
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleToolCallTimeout
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleToolCallTimeout(
    const DialogueState& state,
    const ToolCallTimeout& event) const {
  auto next = state;
  next.deliberation = DeliberationPhase::kThinking;
  next.pending_tool_call_ids.clear();
  next.pending_tool_results.clear();

  std::vector<DialogueEffect> effects;
  effects.push_back(RecordTimeoutResults{event.missing_tool_call_ids});
  effects.push_back(StartLlmContinuation{event.now});

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleTimerExpired
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleTimerExpired(
    const DialogueState& state,
    const TimerExpired& event) const {
  switch (event.kind) {
    case TimerKind::kDebounce:
      return HandleDebounceCooldownExpired(state, event.now);
    case TimerKind::kToolCallTimeout: {
      std::vector<std::string> missing;
      for (const auto& id : state.pending_tool_call_ids) {
        if (state.pending_tool_results.find(id) ==
            state.pending_tool_results.end()) {
          missing.push_back(id);
        }
      }
      return HandleToolCallTimeout(state, ToolCallTimeout{missing, event.now});
    }
    case TimerKind::kConversationIdle:
      return {state, {}};
    case TimerKind::kAggregationTimeout:
      return {state, {}};
  }
  return {state, {}};
}

// ---------------------------------------------------------------------------
// HandleSystemEvent
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleSystemEvent(
    const DialogueState& state,
    const SystemEventReceived& event) const {
  if (state.deliberation != DeliberationPhase::kIdle) {
    return {state, {}};
  }

  auto next = state;
  next.conversation_active = true;
  next.last_activity = event.now;
  next.deliberation = DeliberationPhase::kThinking;

  std::vector<DialogueEffect> effects;
  effects.push_back(RecordConversationItem{event.item});
  effects.push_back(StartLlmContinuation{event.now});

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleContinuation
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleContinuation(
    const DialogueState& state,
    const ContinuationRequested& event) const {
  if (state.deliberation != DeliberationPhase::kIdle &&
      state.deliberation != DeliberationPhase::kThinking) {
    return {state, {}};
  }

  auto next = state;
  next.deliberation = DeliberationPhase::kThinking;

  return {next, {StartLlmContinuation{event.now}}};
}

// ---------------------------------------------------------------------------
// IsBudgetExhausted
// ---------------------------------------------------------------------------

bool DefaultDialogueReducer::IsBudgetExhausted(
    const DialogueState& state,
    std::chrono::steady_clock::time_point /*now*/) const {
  if (state.turn_prompt_tokens + state.turn_completion_tokens >=
      config_.token_budget) {
    return true;
  }
  if (state.turn_action_count >= config_.action_count_limit) {
    return true;
  }
  if (state.turn_continuation_count >= config_.max_continuations) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// NextTurnTriggerId
// ---------------------------------------------------------------------------

uint64_t DefaultDialogueReducer::NextTurnTriggerId(
    const DialogueState& state) const {
  return state.next_turn_trigger_id + 1;
}

}  // namespace shizuru::core::dialogue
