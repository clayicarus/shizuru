#pragma once

#include "controller/types.h"

namespace shizuru::core {

// Legacy turn-trigger filter interface.
//
// The current runtime path temporarily bypasses semantic turn-trigger
// filtering and treats all meaningful observations as respond-now. The
// interface is kept so we can re-enable classifier-backed reply gating later
// without rewriting the surrounding strategy plumbing.
//
// Default implementation: accept everything (no filtering).
// Voice agent example: use LLM to classify whether the ASR transcript
// is directed at the assistant or is background speech to ignore.
class ObservationFilter {
 public:
  virtual ~ObservationFilter() = default;

  // Returns true if the observation should trigger a response.
  // Returns false if it should be stored without triggering an assistant turn.
  virtual bool ShouldProcess(const Observation& obs) = 0;
};

// Default: accept all observations.
class AcceptAllFilter : public ObservationFilter {
 public:
  bool ShouldProcess(const Observation& /*obs*/) override { return true; }
};

}  // namespace shizuru::core
