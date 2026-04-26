#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "dialogue/types.h"  // TimerKind

namespace shizuru::core::dialogue {

struct TimerEntry {
  TimerKind kind;
  std::string timer_id;
  uint64_t generation;  // Monotonic generation — stale entries are skipped
  std::chrono::steady_clock::time_point deadline;

  bool operator>(const TimerEntry& other) const {
    return deadline > other.deadline;
  }
};

// Generation-safe timer book.
//
// Each timer_id has a current generation counter. Schedule() increments the
// generation and pushes a new entry. Cancel() increments the generation
// without pushing. PopExpired() skips entries whose generation does not
// match the current generation for their timer_id.
//
// This means Schedule("x") → Cancel("x") → Schedule("x") works correctly:
// the second Schedule gets a new generation, and the first entry (now stale)
// is silently skipped on pop.
class TimerBook {
 public:
  // Schedule a timer. If a timer with the same timer_id already exists,
  // the old entry becomes stale (its generation no longer matches).
  void Schedule(TimerKind kind, std::string timer_id,
                std::chrono::steady_clock::time_point deadline);

  // Cancel a timer by timer_id. Increments the generation so any
  // in-heap entry for this id becomes stale.
  void Cancel(const std::string& timer_id);

  // Returns the earliest non-stale deadline, or nullopt if empty.
  std::optional<std::chrono::steady_clock::time_point> NextDeadline() const;

  // Pops all non-stale timers whose deadline <= now. Returns in deadline order.
  std::vector<TimerEntry> PopExpired(
      std::chrono::steady_clock::time_point now);

  bool Empty() const;

 private:
  // Mutable so that const methods (NextDeadline, Empty) can lazily
  // discard stale entries from the top of the heap.
  mutable std::priority_queue<TimerEntry, std::vector<TimerEntry>,
                              std::greater<TimerEntry>> heap_;
  std::unordered_map<std::string, uint64_t> generations_;

  // Discard stale entries from the top of the heap.
  void DiscardStale() const;
};

}  // namespace shizuru::core::dialogue
