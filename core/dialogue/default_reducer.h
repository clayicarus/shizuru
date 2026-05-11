#pragma once

#include <chrono>

#include "controller/config.h"
#include "dialogue/reducer.h"

namespace shizuru::core::dialogue {

class DefaultDialogueReducer : public DialogueReducer {
 public:
  explicit DefaultDialogueReducer(const ControllerConfig& config);

  DialogueDecision Reduce(const DialogueState& state,
                          const DialogueEvent& event) const override;

 private:
  // Immutable config snapshot — no references to Controller.
  ControllerConfig config_;

  // --- Event handlers ---
  DialogueDecision HandleInterrupt(
      const DialogueState& state,
      std::chrono::steady_clock::time_point now) const;

  DialogueDecision HandleDebounceCooldownExpired(
      const DialogueState& state,
      std::chrono::steady_clock::time_point now) const;

  DialogueDecision HandleConversationItemReceived(
      const DialogueState& state,
      const ConversationItemReceived& event) const;

  DialogueDecision HandleSystemEvent(
      const DialogueState& state,
      const SystemEventReceived& event) const;

  DialogueDecision HandleLlmCompleted(
      const DialogueState& state,
      const LlmCompleted& event) const;

  DialogueDecision HandleLlmFailed(
      const DialogueState& state,
      const LlmFailed& event) const;

  DialogueDecision HandleToolResult(
      const DialogueState& state,
      const ToolResultReceived& event) const;

  DialogueDecision HandleToolCallTimeout(
      const DialogueState& state,
      const ToolCallTimeout& event) const;

  DialogueDecision HandleContinuation(
      const DialogueState& state,
      const ContinuationRequested& event) const;

  DialogueDecision HandleTimerExpired(
      const DialogueState& state,
      const TimerExpired& event) const;

  DialogueDecision HandleTurnTriggerClassified(
      const DialogueState& state,
      const TurnTriggerClassified& event) const;

  // Budget check — pure, returns whether any limit is exceeded.
  bool IsBudgetExhausted(const DialogueState& state,
                         std::chrono::steady_clock::time_point now) const;

  // Returns ++state.next_turn_trigger_id (applied to next_state).
  uint64_t NextTurnTriggerId(const DialogueState& state) const;
};

}  // namespace shizuru::core::dialogue
