#include "strategies/llm_observation_filter.h"

#include <algorithm>
#include <cctype>

#include "async_logger.h"
#include "context/types.h"

namespace shizuru::core {

LlmObservationFilter::LlmObservationFilter(std::unique_ptr<LlmClient> llm,
                                           std::string system_prompt)
    : llm_(std::move(llm)), system_prompt_(std::move(system_prompt)) {}

bool LlmObservationFilter::ShouldProcess(const Observation& obs) {
  // Only filter user messages.  Tool results, system events, etc. always pass.
  if (obs.type != ObservationType::kUserMessage) return true;

  // Empty content — skip (likely an interrupt signal).
  if (obs.content.empty()) return true;

  // Build a minimal context window for classification.
  ContextWindow window;
  window.estimated_tokens = 100;

  ContextMessage sys_msg;
  sys_msg.role = "system";
  sys_msg.content = system_prompt_;
  window.messages.push_back(std::move(sys_msg));

  ContextMessage user_msg;
  user_msg.role = "user";
  user_msg.content = obs.content;
  window.messages.push_back(std::move(user_msg));

  try {
    auto result = llm_->Submit(window);
    const std::string& answer = result.candidate.response_text;

    // Normalize: lowercase, trim whitespace.
    std::string normalized;
    for (char c : answer) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        normalized += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
      }
    }

    bool should_process = normalized.find("yes") == 0;
    LOG_INFO("[ObsFilter] \"{}\" → {} (raw: \"{}\")",
             obs.content, should_process ? "PROCESS" : "SKIP", answer);
    return should_process;
  } catch (const std::exception& e) {
    // On error, default to processing (don't silently drop user input).
    LOG_WARN("[ObsFilter] LLM error, defaulting to PROCESS: {}", e.what());
    return true;
  }
}

std::string LlmObservationFilter::DefaultPrompt() {
  return
      "You are a voice input classifier for a companion assistant. "
      "You will receive a transcript from speech recognition.\n\n"
      "Determine if this transcript is something the assistant should respond to.\n\n"
      "Reply ONLY \"yes\" or \"no\".\n\n"
      "Answer \"yes\" for:\n"
      "- Questions, requests, or statements\n"
      "- Emotional expressions (sighs, complaints, frustration, tiredness)\n"
      "- Short but meaningful messages (\"好的\", \"停\", \"help\", \"烦死了\", \"唉\")\n"
      "- Anything that sounds like the user is talking to the assistant\n\n"
      "Answer \"no\" ONLY for:\n"
      "- Clear background noise artifacts (random syllables, TV audio)\n"
      "- Obvious speech recognition errors (garbled text)\n\n"
      "When in doubt, answer \"yes\". It is better to respond to noise "
      "than to ignore a user who needs support.";
}

}  // namespace shizuru::core
