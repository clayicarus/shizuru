#pragma once

// core/strategies/window_aggregator.h — Time-window observation aggregator.
//
// Collects messages within a configurable time window.  Each message retains
// its own ConversationItem (actor_id, actor_name, text).  When the window
// expires without new input, all buffered items are flushed as a single
// Observation with a populated `items` vector.
//
// Designed for multi-user text chat (e.g., group chat via OneBot) where
// multiple users may send messages in quick succession and we want to
// batch them into one LLM turn.

#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

#include "conversation/item.h"
#include "strategies/observation_aggregator.h"

namespace shizuru::core {

struct WindowAggregatorConfig {
  // Time to wait after the last message before flushing.
  std::chrono::milliseconds window_duration{3000};
};

class WindowAggregator : public ObservationAggregator {
 public:
  explicit WindowAggregator(WindowAggregatorConfig config = {});

  std::optional<Observation> Feed(const Observation& obs) override;
  std::optional<Observation> CheckTimeout() override;
  bool HasPending() const override;
  void Reset() override;

 private:
  Observation Flush();

  WindowAggregatorConfig config_;

  mutable std::mutex mu_;
  std::vector<conversation::ConversationItem> buffered_items_;
  std::vector<std::string> buffered_contents_;
  std::chrono::steady_clock::time_point last_input_time_;
  std::string last_source_;
};

}  // namespace shizuru::core
