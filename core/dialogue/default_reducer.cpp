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
      return HandleUserMessageDuringDebounce(state, e);
    },
    [&](const ShutdownRequested&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const LlmCompleted&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const LlmFailed&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const ToolResultReceived&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const ToolCallTimeout&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const ContinuationRequested&) -> DialogueDecision {
      return {state, {}};
    },
    [&](const SystemEventReceived&) -> DialogueDecision {
      return {state, {}};
    },
  }, event);
}

DialogueDecision DefaultDialogueReducer::HandleInterrupt(
    const DialogueState& state,
    std::chrono::steady_clock::time_point now) const {
  auto next = state;
  next.cooldown = CooldownPhase::kDebouncing;
  next.last_activity = now;

  std::vector<DialogueEffect> effects;
  effects.push_back(CancelLlm{});

  return {next, effects};
}

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

DialogueDecision DefaultDialogueReducer::HandleUserMessageDuringDebounce(
    const DialogueState& state,
    const UserMessageReceived& event) const {
  if (state.cooldown == CooldownPhase::kDebouncing) {
    std::vector<DialogueEffect> effects;
    effects.push_back(RecordMemory{event.observation});
    return {state, effects};
  }

  // cooldown == kNone: no-op — Controller handles normal flow in Phase 1.
  return {state, {}};
}

bool DefaultDialogueReducer::IsBudgetExhausted(
    const DialogueState& state,
    std::chrono::steady_clock::time_point now) const {
  if (state.turn_count >= config_.max_turns) {
    return true;
  }
  if (state.total_prompt_tokens + state.total_completion_tokens >=
      config_.token_budget) {
    return true;
  }
  if (state.action_count >= config_.action_count_limit) {
    return true;
  }
  if (now - state.session_start >= config_.turn_timeout) {
    return true;
  }
  return false;
}

}  // namespace shizuru::core::dialogue
