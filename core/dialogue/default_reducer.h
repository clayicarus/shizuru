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

  // Per-event handlers (all const, all pure).
  DialogueDecision HandleInterrupt(
      const DialogueState& state,
      std::chrono::steady_clock::time_point now) const;

  DialogueDecision HandleDebounceCooldownExpired(
      const DialogueState& state,
      std::chrono::steady_clock::time_point now) const;

  DialogueDecision HandleUserMessageDuringDebounce(
      const DialogueState& state,
      const UserMessageReceived& event) const;

  // Budget check — pure, returns whether any limit is exceeded.
  bool IsBudgetExhausted(const DialogueState& state,
                         std::chrono::steady_clock::time_point now) const;
};

}  // namespace shizuru::core::dialogue
