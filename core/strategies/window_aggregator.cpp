// core/strategies/window_aggregator.cpp — Time-window observation aggregator.

#include "strategies/window_aggregator.h"

#include <sstream>

#include "async_logger.h"
#include "conversation/item.h"

namespace shizuru::core {

WindowAggregator::WindowAggregator(WindowAggregatorConfig config)
    : config_(config) {}

std::optional<Observation> WindowAggregator::Feed(const Observation& obs) {
  // Only aggregate user messages.  Other types pass through immediately.
  if (obs.type != ObservationType::kUserMessage) {
    return obs;
  }

  std::lock_guard<std::mutex> lock(mu_);

  // Buffer the ConversationItem (preserves actor_id, actor_name, text).
  if (obs.item.has_value()) {
    buffered_items_.push_back(*obs.item);
  } else {
    // Fallback: construct a minimal item.
    buffered_items_.push_back(
        conversation::MakeHumanMessageItem(
            obs.source.empty() ? "user" : obs.source, "", obs.content));
  }
  buffered_contents_.push_back(obs.content);
  last_input_time_ = std::chrono::steady_clock::now();
  last_source_ = obs.source;

  LOG_INFO("[WindowAggregator] Buffered message from {} (total: {})",
           obs.source, buffered_items_.size());

  return std::nullopt;  // Always buffer, flush on timeout.
}

std::optional<Observation> WindowAggregator::CheckTimeout() {
  std::lock_guard<std::mutex> lock(mu_);
  if (buffered_items_.empty()) { return std::nullopt; }

  auto elapsed = std::chrono::steady_clock::now() - last_input_time_;
  if (elapsed < config_.window_duration) { return std::nullopt; }

  LOG_INFO("[WindowAggregator] Window expired ({}ms), flushing {} messages",
           std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
           buffered_items_.size());
  return Flush();
}

bool WindowAggregator::HasPending() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !buffered_items_.empty();
}

void WindowAggregator::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  buffered_items_.clear();
  buffered_contents_.clear();
}

Observation WindowAggregator::Flush() {
  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.timestamp = std::chrono::steady_clock::now();
  obs.source = last_source_;

  // Build a combined content string for logging / legacy paths.
  std::ostringstream oss;
  for (size_t i = 0; i < buffered_contents_.size(); ++i) {
    if (i > 0) { oss << "\n"; }
    oss << buffered_contents_[i];
  }
  obs.content = oss.str();

  // Move all buffered items into the observation.
  obs.items = std::move(buffered_items_);

  // For single-message case, also set the legacy `item` field for
  // backward compatibility with code that checks obs.item.
  if (obs.items.size() == 1) {
    obs.item = obs.items[0];
  }

  buffered_items_.clear();
  buffered_contents_.clear();

  return obs;
}

}  // namespace shizuru::core
