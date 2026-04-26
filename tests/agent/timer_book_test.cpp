// Unit + property-based tests for TimerBook — generation-safe timer data
// structure.
//
// Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.8, 19.2

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>

#include "dialogue/timer_book.h"
#include "dialogue/types.h"

namespace shizuru::core::dialogue {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// Base time_point for predictable testing. All deadlines use offsets from this.
const TimePoint kBase = TimePoint{std::chrono::seconds(1000)};

TimePoint At(int offset_ms) {
  return kBase + std::chrono::milliseconds(offset_ms);
}

// ===========================================================================
// Unit tests
// ===========================================================================

TEST(TimerBookTest, ScheduleAndNextDeadline) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "timer_a", At(100));
  auto deadline = book.NextDeadline();
  ASSERT_TRUE(deadline.has_value());
  EXPECT_EQ(*deadline, At(100));
}

TEST(TimerBookTest, ScheduleAndPopExpired) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "timer_a", At(100));
  auto expired = book.PopExpired(At(100));
  ASSERT_EQ(expired.size(), 1U);
  EXPECT_EQ(expired[0].timer_id, "timer_a");
  EXPECT_EQ(expired[0].kind, TimerKind::kDebounce);
  EXPECT_EQ(expired[0].deadline, At(100));
}

TEST(TimerBookTest, CancelledEntryNotReturned) {
  TimerBook book;
  book.Schedule(TimerKind::kToolCallTimeout, "timer_b", At(200));
  book.Cancel("timer_b");
  auto expired = book.PopExpired(At(300));
  EXPECT_TRUE(expired.empty());
}

TEST(TimerBookTest, NextDeadlineReturnsEarliest) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "late", At(500));
  book.Schedule(TimerKind::kToolCallTimeout, "early", At(100));
  book.Schedule(TimerKind::kConversationIdle, "mid", At(300));
  auto deadline = book.NextDeadline();
  ASSERT_TRUE(deadline.has_value());
  EXPECT_EQ(*deadline, At(100));
}

TEST(TimerBookTest, PopExpiredReturnsInDeadlineOrder) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "c", At(300));
  book.Schedule(TimerKind::kToolCallTimeout, "a", At(100));
  book.Schedule(TimerKind::kConversationIdle, "b", At(200));
  auto expired = book.PopExpired(At(400));
  ASSERT_EQ(expired.size(), 3U);
  EXPECT_EQ(expired[0].timer_id, "a");
  EXPECT_EQ(expired[1].timer_id, "b");
  EXPECT_EQ(expired[2].timer_id, "c");
  EXPECT_LE(expired[0].deadline, expired[1].deadline);
  EXPECT_LE(expired[1].deadline, expired[2].deadline);
}

TEST(TimerBookTest, EmptyWhenNoTimers) {
  TimerBook book;
  EXPECT_TRUE(book.Empty());
}

TEST(TimerBookTest, EmptyWhenAllCancelled) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "x", At(100));
  book.Schedule(TimerKind::kToolCallTimeout, "y", At(200));
  book.Cancel("x");
  book.Cancel("y");
  EXPECT_TRUE(book.Empty());
}

TEST(TimerBookTest, NotEmptyWhenActiveTimers) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "x", At(100));
  EXPECT_FALSE(book.Empty());
}

TEST(TimerBookTest, PopExpiredMixCancelledAndActive) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "keep_1", At(100));
  book.Schedule(TimerKind::kToolCallTimeout, "cancel_me", At(150));
  book.Schedule(TimerKind::kConversationIdle, "keep_2", At(200));
  book.Cancel("cancel_me");
  auto expired = book.PopExpired(At(300));
  ASSERT_EQ(expired.size(), 2U);
  EXPECT_EQ(expired[0].timer_id, "keep_1");
  EXPECT_EQ(expired[1].timer_id, "keep_2");
}

TEST(TimerBookTest, GenerationSafety_RescheduleAfterCancel) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "reused_id", At(100));
  book.Cancel("reused_id");
  book.Schedule(TimerKind::kDebounce, "reused_id", At(200));
  auto expired = book.PopExpired(At(300));
  ASSERT_EQ(expired.size(), 1U);
  EXPECT_EQ(expired[0].timer_id, "reused_id");
  EXPECT_EQ(expired[0].deadline, At(200));
}

TEST(TimerBookTest, GenerationSafety_NextDeadlineReflectsReschedule) {
  TimerBook book;
  book.Schedule(TimerKind::kDebounce, "reused_id", At(100));
  book.Cancel("reused_id");
  book.Schedule(TimerKind::kDebounce, "reused_id", At(200));
  auto deadline = book.NextDeadline();
  ASSERT_TRUE(deadline.has_value());
  EXPECT_EQ(*deadline, At(200));
}

// ===========================================================================
// Property-based tests
// ===========================================================================

// ---------------------------------------------------------------------------
// Property 3 (Task 7.3): TimerBook schedule-then-pop round trip
// For any set of timer entries, scheduling all then PopExpired(now >= max
// deadlines) returns exactly the non-cancelled entries in deadline order.
// **Validates: Requirements 4.1, 4.4, 4.5**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(TimerBookPropTest, prop_schedule_then_pop_round_trip, (void)) {
  TimerBook book;

  int n = *rc::gen::inRange(1, 11);
  struct Entry {
    std::string id;
    TimerKind kind;
    int offset_ms;
    bool cancelled;
  };
  std::vector<Entry> entries;

  for (int i = 0; i < n; ++i) {
    Entry e;
    e.id = "timer_" + std::to_string(i);
    e.kind = *rc::gen::element(
        TimerKind::kDebounce, TimerKind::kToolCallTimeout,
        TimerKind::kConversationIdle);
    e.offset_ms = *rc::gen::inRange(1, 10000);
    e.cancelled = *rc::gen::arbitrary<bool>();
    entries.push_back(e);
  }

  int max_offset = 0;
  for (const auto& e : entries) {
    book.Schedule(e.kind, e.id, At(e.offset_ms));
    max_offset = std::max(max_offset, e.offset_ms);
  }

  for (const auto& e : entries) {
    if (e.cancelled) {
      book.Cancel(e.id);
    }
  }

  auto expired = book.PopExpired(At(max_offset + 1));

  std::vector<Entry> expected;
  for (const auto& e : entries) {
    if (!e.cancelled) {
      expected.push_back(e);
    }
  }

  RC_ASSERT(expired.size() == expected.size());

  for (size_t i = 1; i < expired.size(); ++i) {
    RC_ASSERT(expired[i - 1].deadline <= expired[i].deadline);
  }

  std::set<std::string> expired_ids;
  for (const auto& e : expired) {
    expired_ids.insert(e.timer_id);
  }
  std::set<std::string> expected_ids;
  for (const auto& e : expected) {
    expected_ids.insert(e.id);
  }
  RC_ASSERT(expired_ids == expected_ids);
}

// ---------------------------------------------------------------------------
// Property 4 (Task 7.4): TimerBook cancel exclusion
// For any scheduled-then-cancelled timer, PopExpired never returns it and
// NextDeadline doesn't return its deadline when it's the only timer.
// **Validates: Requirements 4.2, 4.3, 4.6**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(TimerBookPropTest, prop_cancel_exclusion, (void)) {
  TimerBook book;

  auto kind = *rc::gen::element(
      TimerKind::kDebounce, TimerKind::kToolCallTimeout,
      TimerKind::kConversationIdle);
  int offset_ms = *rc::gen::inRange(1, 10000);
  std::string id = "cancelled_timer";

  book.Schedule(kind, id, At(offset_ms));
  book.Cancel(id);

  auto expired = book.PopExpired(At(offset_ms + 10000));
  RC_ASSERT(expired.empty());

  auto deadline = book.NextDeadline();
  RC_ASSERT(!deadline.has_value());

  RC_ASSERT(book.Empty());
}

// ---------------------------------------------------------------------------
// Property 18 (Task 7.14): Timer id reuse after cancel
// Schedule(id, d1) → Cancel(id) → Schedule(id, d2) → PopExpired(max(d1,d2))
// returns only the second entry.
// **Validates: Requirements 4.1, 4.2, 4.8**
// ---------------------------------------------------------------------------

RC_GTEST_PROP(TimerBookPropTest, prop_timer_id_reuse_after_cancel, (void)) {
  TimerBook book;

  auto kind = *rc::gen::element(
      TimerKind::kDebounce, TimerKind::kToolCallTimeout,
      TimerKind::kConversationIdle);
  int d1_ms = *rc::gen::inRange(1, 10000);
  int d2_ms = *rc::gen::inRange(1, 10000);
  std::string id = "reused_id";

  book.Schedule(kind, id, At(d1_ms));
  book.Cancel(id);
  book.Schedule(kind, id, At(d2_ms));

  int max_d = std::max(d1_ms, d2_ms);
  auto expired = book.PopExpired(At(max_d + 1));

  RC_ASSERT(expired.size() == 1);
  RC_ASSERT(expired[0].timer_id == id);
  RC_ASSERT(expired[0].deadline == At(d2_ms));
}

}  // namespace
}  // namespace shizuru::core::dialogue
