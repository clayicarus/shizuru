#pragma once

#include <memory>
#include <string>

#include "interfaces/llm_client.h"
#include "strategies/observation_filter.h"

namespace shizuru::core {

// LLM-based observation filter.
// Uses a (typically lightweight) LLM to classify whether an ASR transcript
// warrants a response from the agent.
//
// The filter sends a short classification prompt and expects the LLM to
// respond with "yes" or "no".  Any response not starting with "yes" is
// treated as "no" (reject).
//
// The filter owns its own LlmClient — independent from the main reasoning
// LLM.  This can be the same endpoint with a cheaper model, or a completely
// separate service.
class LlmObservationFilter : public ObservationFilter {
 public:
  explicit LlmObservationFilter(std::unique_ptr<LlmClient> llm,
                                std::string system_prompt = DefaultPrompt());

  bool ShouldProcess(const Observation& obs) override;

  static std::string DefaultPrompt();

 private:
  std::unique_ptr<LlmClient> llm_;
  std::string system_prompt_;
};

}  // namespace shizuru::core
