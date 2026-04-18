#pragma once

#include "dialogue/types.h"

namespace shizuru::core::dialogue {

// Pure virtual reducer interface.
// Contract: (state, event) → (next_state, effects).
// Implementations must not perform I/O, block, or mutate shared state.
class DialogueReducer {
 public:
  virtual ~DialogueReducer() = default;

  virtual DialogueDecision Reduce(const DialogueState& state,
                                  const DialogueEvent& event) const = 0;
};

}  // namespace shizuru::core::dialogue
