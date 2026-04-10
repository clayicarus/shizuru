#include "strategies/llm_observation_aggregator.h"

#include <algorithm>
#include <cctype>
#include <future>

#include "async_logger.h"
#include "context/types.h"

namespace shizuru::core {

LlmObservationAggregator::LlmObservationAggregator(
    std::unique_ptr<LlmClient> llm, LlmAggregatorConfig config)
    : llm_(std::move(llm)), config_(std::move(config)) {
  if (config_.system_prompt.empty()) {
    config_.system_prompt = DefaultPrompt();
  }
}

std::optional<Observation> LlmObservationAggregator::Feed(
    const Observation& obs) {
  // Only aggregate user messages.
  if (obs.type != ObservationType::kUserMessage) return obs;
  if (obs.content.empty()) return obs;

  std::lock_guard<std::mutex> lock(mu_);

  if (!buffer_.empty()) buffer_ += " ";
  buffer_ += obs.content;
  last_input_time_ = std::chrono::steady_clock::now();
  has_pending_ = true;
  source_ = obs.source;

  LOG_INFO("[Aggregator] Buffered: \"{}\" (total: \"{}\")",
           obs.content, buffer_);

  bool complete = IsUtteranceComplete(buffer_);
  if (complete) {
    LOG_INFO("[Aggregator] Utterance complete, flushing: \"{}\"", buffer_);
    return FlushBuffer();
  }

  LOG_INFO("[Aggregator] Utterance incomplete, waiting for more");
  return std::nullopt;
}

std::optional<Observation> LlmObservationAggregator::CheckTimeout() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!has_pending_) return std::nullopt;

  auto elapsed = std::chrono::steady_clock::now() - last_input_time_;
  if (elapsed < config_.aggregation_timeout) return std::nullopt;

  LOG_INFO("[Aggregator] Timeout ({}ms), flushing: \"{}\"",
           std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
           buffer_);
  return FlushBuffer();
}

bool LlmObservationAggregator::HasPending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return has_pending_;
}

void LlmObservationAggregator::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  buffer_.clear();
  has_pending_ = false;
}

bool LlmObservationAggregator::IsUtteranceComplete(const std::string& text) {
  ContextWindow window;
  window.estimated_tokens = 100;

  ContextMessage sys_msg;
  sys_msg.role = "system";
  sys_msg.content = config_.system_prompt;
  window.messages.push_back(std::move(sys_msg));

  ContextMessage user_msg;
  user_msg.role = "user";
  user_msg.content = text;
  window.messages.push_back(std::move(user_msg));

  auto future = std::async(std::launch::async, [&]() -> LlmResult {
    return llm_->Submit(window);
  });

  auto status = future.wait_for(config_.llm_timeout);
  if (status == std::future_status::timeout) {
    LOG_WARN("[Aggregator] LLM timeout, defaulting to complete");
    llm_->Cancel();
    return true;
  }

  try {
    auto result = future.get();
    const std::string& answer = result.candidate.response_text;
    std::string normalized;
    for (char c : answer) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        normalized += static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
      }
    }
    bool complete = normalized.find("yes") == 0;
    LOG_INFO("[Aggregator] LLM says: \"{}\" -> {}",
             answer, complete ? "COMPLETE" : "INCOMPLETE");
    return complete;
  } catch (const std::exception& e) {
    LOG_WARN("[Aggregator] LLM error, defaulting to complete: {}", e.what());
    return true;
  }
}

Observation LlmObservationAggregator::FlushBuffer() {
  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = std::move(buffer_);
  obs.source = source_;
  obs.timestamp = std::chrono::steady_clock::now();
  buffer_.clear();
  has_pending_ = false;
  return obs;
}

std::string LlmObservationAggregator::DefaultPrompt() {
  return
      "You are a speech endpointing classifier for a voice assistant. "
      "You receive a transcript from speech recognition.\n\n"
      "Determine if the user's input is CLEARLY INCOMPLETE (they are "
      "obviously mid-sentence and will continue speaking).\n\n"
      "Reply ONLY \"yes\" (input can be processed now) or \"no\" "
      "(input is clearly incomplete, wait for more).\n\n"
      "Answer \"no\" ONLY for obviously incomplete fragments:\n"
      "- Trailing conjunctions/particles: \"我想要...\" \"如果明天\" \"because\"\n"
      "- Dangling clauses: \"请帮我把那个\" \"Can you open the\"\n"
      "- Mid-word cutoffs or clearly unfinished thoughts\n\n"
      "Answer \"yes\" for everything else, including:\n"
      "- Complete questions: \"为什么？\" \"What time is it\"\n"
      "- Short but valid utterances: \"好的\" \"停\" \"yes\" \"help\"\n"
      "- Filler words: \"嗯\" \"啊\" (these should be processed, not buffered)\n"
      "- Anything ambiguous — when in doubt, answer \"yes\"";
}

}  // namespace shizuru::core
