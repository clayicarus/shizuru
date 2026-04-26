#include "dialogue/timer_book.h"

#include <utility>

namespace shizuru::core::dialogue {

void TimerBook::Schedule(TimerKind kind, std::string timer_id,
                         std::chrono::steady_clock::time_point deadline) {
  uint64_t gen = ++generations_[timer_id];
  heap_.push(TimerEntry{kind, std::move(timer_id), gen, deadline});
}

void TimerBook::Cancel(const std::string& timer_id) {
  ++generations_[timer_id];
}

std::optional<std::chrono::steady_clock::time_point>
TimerBook::NextDeadline() const {
  DiscardStale();
  if (heap_.empty()) {
    return std::nullopt;
  }
  return heap_.top().deadline;
}

std::vector<TimerEntry> TimerBook::PopExpired(
    std::chrono::steady_clock::time_point now) {
  std::vector<TimerEntry> result;
  while (!heap_.empty()) {
    const auto& top = heap_.top();
    // Skip stale entries.
    auto it = generations_.find(top.timer_id);
    if (it == generations_.end() || top.generation != it->second) {
      heap_.pop();
      continue;
    }
    // Stop if the earliest non-stale entry is in the future.
    if (top.deadline > now) {
      break;
    }
    result.push_back(top);
    heap_.pop();
  }
  return result;
}

bool TimerBook::Empty() const {
  DiscardStale();
  return heap_.empty();
}

void TimerBook::DiscardStale() const {
  while (!heap_.empty()) {
    const auto& top = heap_.top();
    auto it = generations_.find(top.timer_id);
    if (it != generations_.end() && top.generation == it->second) {
      break;  // Top entry is current — stop.
    }
    heap_.pop();
  }
}

}  // namespace shizuru::core::dialogue
