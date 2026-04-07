#pragma once

#include "controller/types.h"

namespace shizuru::core {

// Decides whether an incoming observation should be processed by the
// Controller or silently dropped.
//
// Injection point: Controller::RunLoop, before HandleThinking.
//
// Default implementation: accept everything (no filtering).
// Voice agent example: use LLM to classify whether the ASR transcript
// is directed at the assistant or is background speech to ignore.
class ObservationFilter {
 public:
  virtual ~ObservationFilter() = default;

  // Returns true if the observation should be processed.
  // Returns false if it should be silently ignored (Controller stays in
  // kListening without transitioning to kThinking).
  virtual bool ShouldProcess(const Observation& obs) = 0;
};

// Default: accept all observations.
class AcceptAllFilter : public ObservationFilter {
 public:
  bool ShouldProcess(const Observation& /*obs*/) override { return true; }
};

}  // namespace shizuru::core
