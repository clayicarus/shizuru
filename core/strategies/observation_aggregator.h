#pragma once

#include <chrono>
#include <optional>

#include "controller/types.h"

namespace shizuru::core {

// Aggregates multiple ASR transcript fragments into a single complete
// observation.  Decides when the user has finished speaking (endpointing).
//
// Injection point: Controller::RunLoop, before ObservationFilter.
//
// Flow:
//   1. ASR text arrives → Feed() appends to buffer, returns nullopt or value
//   2. If the aggregator decides the user is done → returns the merged observation
//   3. If not done → returns nullopt, Controller waits for more
//   4. CheckTimeout() is called periodically — force-flushes after a delay
//
// Default implementation: no aggregation (pass through immediately).
class ObservationAggregator {
 public:
  virtual ~ObservationAggregator() = default;

  // Feed an observation into the aggregator.
  // Returns the aggregated observation when the user is done speaking,
  // or nullopt to keep buffering.
  virtual std::optional<Observation> Feed(const Observation& obs) = 0;

  // Called periodically when idle.  Returns buffered content if timeout
  // has elapsed, or nullopt if still waiting.
  virtual std::optional<Observation> CheckTimeout() = 0;

  // Returns true if there is buffered content waiting.
  virtual bool HasPending() const = 0;

  // Reset internal state (e.g., on interrupt).
  virtual void Reset() = 0;
};

// Default: no aggregation, pass through immediately.
class PassthroughAggregator : public ObservationAggregator {
 public:
  std::optional<Observation> Feed(const Observation& obs) override {
    return obs;
  }
  std::optional<Observation> CheckTimeout() override { return std::nullopt; }
  bool HasPending() const override { return false; }
  void Reset() override {}
};

}  // namespace shizuru::core
