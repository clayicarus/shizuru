#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "interfaces/llm_client.h"
#include "strategies/observation_aggregator.h"

namespace shizuru::core {

struct LlmAggregatorConfig {
  // Max time to wait for more input before force-flushing.
  std::chrono::milliseconds aggregation_timeout{5000};

  // Max time for the endpointing LLM call.
  std::chrono::milliseconds llm_timeout{2000};

  // System prompt for the endpointing classifier.
  std::string system_prompt;
};

// LLM-based observation aggregator with endpointing.
//
// Buffers ASR fragments and uses a lightweight LLM to determine whether
// the user has finished speaking.  Only clearly incomplete fragments
// (mid-sentence cutoffs) are buffered; everything else passes through.
//
// Owns its own LlmClient, independent from the main reasoning LLM.
class LlmObservationAggregator : public ObservationAggregator {
 public:
  explicit LlmObservationAggregator(std::unique_ptr<LlmClient> llm,
                                    LlmAggregatorConfig config = {});

  std::optional<Observation> Feed(const Observation& obs) override;
  std::optional<Observation> CheckTimeout() override;
  bool HasPending() const override;
  void Reset() override;

  static std::string DefaultPrompt();

 private:
  bool IsUtteranceComplete(const std::string& text);
  Observation FlushBuffer();

  std::unique_ptr<LlmClient> llm_;
  LlmAggregatorConfig config_;

  mutable std::mutex mu_;
  std::string buffer_;
  std::chrono::steady_clock::time_point last_input_time_;
  bool has_pending_{false};
  std::string source_;
};

}  // namespace shizuru::core
