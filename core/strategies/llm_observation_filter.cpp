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
      "You are a voice input classifier. The user is speaking to a voice "
      "assistant. You will receive a transcript from speech recognition.\n\n"
      "Determine if this transcript is a meaningful message that the "
      "assistant should respond to.\n\n"
      "Reply ONLY \"yes\" or \"no\".\n\n"
      "Answer \"no\" for:\n"
      "- Filler words, interjections, or meaningless sounds "
      "(e.g., \"啊\", \"嗯\", \"哦\", \"呃\", \"um\", \"uh\", \"hmm\")\n"
      "- Background noise transcription artifacts\n"
      "- Incomplete fragments that are clearly not directed at the assistant\n\n"
      "Answer \"yes\" for:\n"
      "- Questions, requests, or statements directed at the assistant\n"
      "- Even short but meaningful messages (e.g., \"好的\", \"停\", \"yes\", \"help\")";
}

}  // namespace shizuru::core
