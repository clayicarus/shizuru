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
// Reduce — dispatch via std::visit (Task 4.12)
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
    [&](const UserMessageReceived& e) {
      return HandleUserMessage(state, e);
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
    [&](const ToolResultReceived& e) {
      return HandleToolResult(state, e);
    },
    [&](const ToolCallTimeout& e) {
      return HandleToolCallTimeout(state, e);
    },
    [&](const ContinuationRequested& e) {
      return HandleContinuation(state, e);
    },
    [&](const SystemEventReceived& e) {
      return HandleSystemEvent(state, e);
    },
    [&](const TimerExpired& e) {
      return HandleTimerExpired(state, e);
    },
    [&](const TurnTriggerClassified& e) {
      return HandleTurnTriggerClassified(state, e);
    },
    [&](const UserFragmentReceived& e) {
      return HandleUserFragmentReceived(state, e);
    },
    [&](const AggregationComplete& e) {
      return HandleAggregationComplete(state, e);
    },
    [&](const AggregationTimeout& e) {
      return HandleAggregationTimeout(state, e);
    },
  }, event);
}

// ---------------------------------------------------------------------------
// HandleInterrupt (Task 4.8 — updated from Phase 1)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleInterrupt(
    const DialogueState& state,
    std::chrono::steady_clock::time_point now) const {
  auto next = state;
  next.cooldown = CooldownPhase::kDebouncing;
  next.last_activity = now;
  next.deliberation = DeliberationPhase::kIdle;

  // Clear pending turn-trigger if any (defensive — kAwaitingTurnTrigger
  // is not reachable via InterruptRequested per ingress precondition).
  next.pending_turn_trigger_id = 0;

  // Clear pending tool state.
  next.pending_tool_call_ids.clear();
  next.pending_tool_results.clear();

  std::vector<DialogueEffect> effects;

  // Phase 3: handle workspace before other interrupt effects.
  // Commit user fragments so they are preserved in history.
  // Discard assistant partial (interrupted output should not be committed).
  // The workspace stays populated in next_state so effect handlers can read it.
  if (!state.workspace.user_fragments.empty()) {
    effects.push_back(CommitWorkspace{true});
  }
  if (state.workspace.assistant_partial.has_value()) {
    effects.push_back(DiscardWorkspace{});
  }
  // If workspace was empty, still clear it defensively via next_state.
  // (Effect handlers also clear it, but this ensures consistency.)
  // NOTE: We do NOT clear next_state.workspace here — the effect handlers
  // (CommitWorkspace / DiscardWorkspace) will clear dialogue_state_.workspace
  // when they execute.

  effects.push_back(CancelLlm{});
  effects.push_back(RecordInterruptMemory{});
  effects.push_back(ScheduleTimer{
      TimerKind::kDebounce,
      "debounce",
      now + config_.debounce_duration});

  // If we were awaiting tool results, cancel the tool call timeout timer.
  if (state.deliberation == DeliberationPhase::kAwaitingToolResults) {
    effects.push_back(CancelTimer{"tool_call_timeout"});
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleDebounceCooldownExpired (Phase 1 — unchanged)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleDebounceCooldownExpired(
    const DialogueState& state,
    std::chrono::steady_clock::time_point now) const {
  auto next = state;
  next.cooldown = CooldownPhase::kNone;

  std::vector<DialogueEffect> effects;

  // Phase 3: commit workspace fragments (merged) before continuation.
  // The workspace stays populated in next_state so the CommitWorkspace effect
  // handler can read it.  The effect handler clears the workspace after commit.
  effects.push_back(CommitWorkspace{true});

  if (IsBudgetExhausted(next, now)) {
    effects.push_back(SignalBudgetExhausted{});
  } else {
    effects.push_back(StartLlmContinuation{now});
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleUserMessage (Task 4.2 — renamed from HandleUserMessageDuringDebounce)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleUserMessage(
    const DialogueState& state,
    const UserMessageReceived& event) const {
  // --- Debounce path (Phase 3: buffer to workspace instead of RecordMemory) ---
  if (state.cooldown == CooldownPhase::kDebouncing) {
    auto next = state;

    WorkspaceEntry entry;
    entry.item = event.observation.item;
    entry.timestamp = event.now;
    next.workspace.user_fragments.push_back(entry);

    std::vector<DialogueEffect> effects;
    effects.push_back(BufferToWorkspace{entry, true});
    return {next, effects};
  }

  // --- Normal path (cooldown == kNone) ---

  // Superseding message during kAwaitingTurnTrigger:
  // Cancel the old turn-trigger classification, then process normally.
  if (state.deliberation == DeliberationPhase::kAwaitingTurnTrigger) {
    auto next = state;
    next.conversation_active = true;
    next.last_activity = event.now;

    std::vector<DialogueEffect> effects;

    // Cancel the old classification.
    effects.push_back(CancelTurnTriggerClassification{
        state.pending_turn_trigger_id});

    // Reset per-turn counters for the new turn.
    next.turn_llm_calls = 0;
    next.turn_prompt_tokens = 0;
    next.turn_completion_tokens = 0;
    next.turn_action_count = 0;
    next.turn_continuation_count = 0;

    // Buffer to workspace then commit immediately (Phase 3).
    WorkspaceEntry entry;
    entry.item = event.observation.item;
    entry.timestamp = event.now;
    next.workspace.user_fragments.push_back(entry);
    effects.push_back(BufferToWorkspace{entry, false});
    effects.push_back(CommitWorkspace{false});

    // Start fresh turn-trigger classification with new id.
    uint64_t new_id = NextTurnTriggerId(state);
    next.next_turn_trigger_id = new_id;
    next.pending_turn_trigger_id = new_id;
    next.deliberation = DeliberationPhase::kAwaitingTurnTrigger;

    effects.push_back(StartTurnTriggerClassification{
        new_id, event.observation});

    return {next, effects};
  }

  // Normal idle path (deliberation == kIdle):
  if (state.deliberation == DeliberationPhase::kIdle) {
    auto next = state;
    next.conversation_active = true;
    next.last_activity = event.now;

    std::vector<DialogueEffect> effects;

    // Reset per-turn counters for the new turn.
    next.turn_llm_calls = 0;
    next.turn_prompt_tokens = 0;
    next.turn_completion_tokens = 0;
    next.turn_action_count = 0;
    next.turn_continuation_count = 0;

    // Buffer to workspace then commit immediately (Phase 3).
    WorkspaceEntry entry;
    entry.item = event.observation.item;
    entry.timestamp = event.now;
    next.workspace.user_fragments.push_back(entry);
    effects.push_back(BufferToWorkspace{entry, false});
    effects.push_back(CommitWorkspace{false});

    // Start turn-trigger classification.
    uint64_t new_id = NextTurnTriggerId(state);
    next.next_turn_trigger_id = new_id;
    next.pending_turn_trigger_id = new_id;
    next.deliberation = DeliberationPhase::kAwaitingTurnTrigger;

    effects.push_back(StartTurnTriggerClassification{
        new_id, event.observation});

    return {next, effects};
  }

  // Other deliberation states with kNone cooldown: no-op.
  return {state, {}};
}

// ---------------------------------------------------------------------------
// HandleTurnTriggerClassified (Task 4.3)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleTurnTriggerClassified(
    const DialogueState& state,
    const TurnTriggerClassified& event) const {
  // Stale rejection: obs_id does not match pending.
  if (event.obs_id != state.pending_turn_trigger_id) {
    return {state, {}};
  }

  auto next = state;
  next.pending_turn_trigger_id = 0;

  if (event.verdict == TurnTriggerVerdict::kRespondNow) {
    next.deliberation = DeliberationPhase::kThinking;
    // Build a trigger observation for StartLlm from the original observation.
    // We use a minimal observation since the real content is already recorded.
    Observation trigger;
    trigger.type = ObservationType::kContinuation;
    trigger.timestamp = event.now;
    return {next, {StartLlm{trigger}}};
  }

  // kStoreOnly: message stays in committed history, no assistant turn.
  next.deliberation = DeliberationPhase::kIdle;
  return {next, {EmitActivityEffect{ActivityKind::kInputStored, ""}}};
}

// ---------------------------------------------------------------------------
// HandleLlmCompleted (Task 4.4)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleLlmCompleted(
    const DialogueState& state,
    const LlmCompleted& event) const {
  auto next = state;

  // Per-turn token and LLM call accounting.
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

      // Populate pending tool call ids from the candidate.
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
      // Check if continuation limit is reached.
      if (next.turn_continuation_count >= config_.max_continuations) {
        next.deliberation = DeliberationPhase::kIdle;
        effects.push_back(SignalBudgetExhausted{});
      } else {
        next.deliberation = DeliberationPhase::kThinking;
        Observation trigger;
        trigger.type = ObservationType::kContinuation;
        trigger.timestamp = event.now;
        effects.push_back(StartLlm{trigger});
      }
      break;
    }
  }

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleLlmFailed (Task 4.5)
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
// HandleToolResult (Task 4.6)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleToolResult(
    const DialogueState& state,
    const ToolResultReceived& event) const {
  auto next = state;
  next.last_activity = event.now;
  next.conversation_active = true;

  // Record the result keyed by tool call id from the item payload.
  std::string tool_call_id = event.observation.item.payload.value("tool_call_id", "");
  if (tool_call_id.empty()) {
    tool_call_id = ObservationSource(event.observation);
  }
  next.pending_tool_results[tool_call_id] = 
      event.observation.item.payload.contains("content")
          ? event.observation.item.payload["content"].dump()
          : "";

  std::vector<DialogueEffect> effects;
  effects.push_back(RecordToolResult{event.observation});

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

    // Cancel the tool call timeout timer — all results are in, so the
    // timeout is no longer needed.  Without this, a stale TimerExpired
    // {kToolCallTimeout} would fire later and trigger a spurious
    // RecordTimeoutResults + StartLlm continuation.
    effects.push_back(CancelTimer{"tool_call_timeout"});

    Observation trigger;
    trigger.type = ObservationType::kContinuation;
    trigger.timestamp = event.now;
    effects.push_back(StartLlm{trigger});
  }
  // Otherwise deliberation stays kAwaitingToolResults.

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleToolCallTimeout (Task 4.7)
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

  Observation trigger;
  trigger.type = ObservationType::kContinuation;
  trigger.timestamp = event.now;
  effects.push_back(StartLlm{trigger});

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleTimerExpired (Task 4.9)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleTimerExpired(
    const DialogueState& state,
    const TimerExpired& event) const {
  switch (event.kind) {
    case TimerKind::kDebounce:
      return HandleDebounceCooldownExpired(state, event.now);
    case TimerKind::kToolCallTimeout: {
      // Compute missing tool call ids from pending state.
      std::vector<std::string> missing;
      for (const auto& id : state.pending_tool_call_ids) {
        if (state.pending_tool_results.find(id) ==
            state.pending_tool_results.end()) {
          missing.push_back(id);
        }
      }
      return HandleToolCallTimeout(
          state, ToolCallTimeout{missing, event.now});
    }
    case TimerKind::kConversationIdle:
      // Declared for forward compatibility — not emitted in Phase 2.
      return {state, {}};
    case TimerKind::kAggregationTimeout:
      // Phase 3: The reducer cannot call the aggregator (purity).
      // Controller's timer handler calls CheckTimeout() and feeds the
      // result back as AggregationTimeout.  Return no-op here.
      return {state, {}};
  }
  // Unreachable, but satisfy compiler.
  return {state, {}};
}

// ---------------------------------------------------------------------------
// HandleSystemEvent (Task 4.10)
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
  effects.push_back(RecordMemory{event.observation});

  Observation trigger;
  trigger.type = ObservationType::kContinuation;
  trigger.timestamp = event.now;
  effects.push_back(StartLlm{trigger});

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleContinuation (Task 4.11)
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

  Observation trigger;
  trigger.type = ObservationType::kContinuation;
  trigger.timestamp = event.now;

  return {next, {StartLlm{trigger}}};
}

// ---------------------------------------------------------------------------
// IsBudgetExhausted (Phase 1 — unchanged)
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

// ---------------------------------------------------------------------------
// HandleUserFragmentReceived (Phase 3 — Task 3.9)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleUserFragmentReceived(
    const DialogueState& state,
    const UserFragmentReceived& event) const {
  auto next = state;
  next.last_activity = event.now;
  next.conversation_active = true;

  WorkspaceEntry entry;
  entry.item = event.observation.item;
  entry.timestamp = event.now;
  next.workspace.user_fragments.push_back(entry);

  std::vector<DialogueEffect> effects;
  effects.push_back(BufferToWorkspace{entry, true});

  return {next, effects};
}

// ---------------------------------------------------------------------------
// HandleAggregationComplete (Phase 3 — Task 3.7)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleAggregationComplete(
    const DialogueState& state,
    const AggregationComplete& event) const {
  UserMessageReceived msg{event.observation, event.now};
  return HandleUserMessage(state, msg);
}

// ---------------------------------------------------------------------------
// HandleAggregationTimeout (Phase 3 — Task 3.8)
// ---------------------------------------------------------------------------

DialogueDecision DefaultDialogueReducer::HandleAggregationTimeout(
    const DialogueState& state,
    const AggregationTimeout& event) const {
  UserMessageReceived msg{event.observation, event.now};
  return HandleUserMessage(state, msg);
}

}  // namespace shizuru::core::dialogue
