#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/scheduler/scheduler_device.h"
#include "io/data_frame.h"

namespace shizuru::app {
namespace {

// Wait up to timeout_ms for predicate to become true.
bool WaitFor(std::function<bool()> pred, int timeout_ms = 3000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) { return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

TEST(SchedulerDeviceTest, FiresItemAtTriggerTime) {
  SchedulerDevice scheduler;

  std::mutex mu;
  std::vector<io::DataFrame> emitted;
  scheduler.SetOutputCallback([&](const std::string&, const std::string&,
                                  io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  scheduler.Start();

  // Schedule an item 100ms from now.
  ScheduledItem item;
  item.id = "r1";
  item.payload = R"({"message":"time to go"})";
  item.trigger_time = std::chrono::system_clock::now() +
                      std::chrono::milliseconds(100);
  scheduler.Schedule(std::move(item));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  scheduler.Stop();

  ASSERT_TRUE(got) << "Scheduler did not fire the item within timeout";
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].type, "scheduler/event");
  std::string payload(emitted[0].payload.begin(), emitted[0].payload.end());
  EXPECT_NE(payload.find("time to go"), std::string::npos);
  EXPECT_EQ(emitted[0].metadata.at("scheduler_item_id"), "r1");
}

TEST(SchedulerDeviceTest, DoesNotFireBeforeTriggerTime) {
  SchedulerDevice scheduler;

  std::atomic<int> fire_count{0};
  scheduler.SetOutputCallback([&](const std::string&, const std::string&,
                                  io::DataFrame) {
    fire_count.fetch_add(1);
  });

  scheduler.Start();

  // Schedule an item 5 seconds from now — should NOT fire during this test.
  ScheduledItem item;
  item.id = "r2";
  item.payload = "future";
  item.trigger_time = std::chrono::system_clock::now() +
                      std::chrono::seconds(5);
  scheduler.Schedule(std::move(item));

  // Wait 300ms — item should not have fired.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  scheduler.Stop();

  EXPECT_EQ(fire_count.load(), 0);
}

TEST(SchedulerDeviceTest, CancelPreventsItemFromFiring) {
  SchedulerDevice scheduler;

  std::atomic<int> fire_count{0};
  scheduler.SetOutputCallback([&](const std::string&, const std::string&,
                                  io::DataFrame) {
    fire_count.fetch_add(1);
  });

  scheduler.Start();

  ScheduledItem item;
  item.id = "r3";
  item.payload = "should not fire";
  item.trigger_time = std::chrono::system_clock::now() +
                      std::chrono::milliseconds(200);
  scheduler.Schedule(std::move(item));

  // Cancel before it fires.
  bool cancelled = scheduler.Cancel("r3");
  EXPECT_TRUE(cancelled);

  // Wait past the trigger time.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  scheduler.Stop();

  EXPECT_EQ(fire_count.load(), 0);
}

TEST(SchedulerDeviceTest, CancelNonexistentReturnsFalse) {
  SchedulerDevice scheduler;
  scheduler.SetOutputCallback([](const std::string&, const std::string&,
                                 io::DataFrame) {});
  scheduler.Start();

  EXPECT_FALSE(scheduler.Cancel("nonexistent"));

  scheduler.Stop();
}

TEST(SchedulerDeviceTest, MultipleItemsFiredInOrder) {
  SchedulerDevice scheduler;

  std::mutex mu;
  std::vector<std::string> fired_ids;
  scheduler.SetOutputCallback([&](const std::string&, const std::string&,
                                  io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    fired_ids.push_back(f.metadata.at("scheduler_item_id"));
  });

  scheduler.Start();

  auto now = std::chrono::system_clock::now();

  // Schedule 3 items: second fires first, then first, then third.
  ScheduledItem a;
  a.id = "a";
  a.payload = "a";
  a.trigger_time = now + std::chrono::milliseconds(200);

  ScheduledItem b;
  b.id = "b";
  b.payload = "b";
  b.trigger_time = now + std::chrono::milliseconds(100);

  ScheduledItem c;
  c.id = "c";
  c.payload = "c";
  c.trigger_time = now + std::chrono::milliseconds(300);

  scheduler.Schedule(std::move(a));
  scheduler.Schedule(std::move(b));
  scheduler.Schedule(std::move(c));

  bool got_all = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return fired_ids.size() >= 3;
  });

  scheduler.Stop();

  ASSERT_TRUE(got_all) << "Not all items fired";
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(fired_ids.size(), 3u);
  // b (100ms) should fire before a (200ms) before c (300ms).
  EXPECT_EQ(fired_ids[0], "b");
  EXPECT_EQ(fired_ids[1], "a");
  EXPECT_EQ(fired_ids[2], "c");
}

TEST(SchedulerDeviceTest, ItemAlreadyPastFiresImmediately) {
  SchedulerDevice scheduler;

  std::mutex mu;
  std::vector<io::DataFrame> emitted;
  scheduler.SetOutputCallback([&](const std::string&, const std::string&,
                                  io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  scheduler.Start();

  // Schedule an item in the past.
  ScheduledItem item;
  item.id = "past";
  item.payload = "overdue";
  item.trigger_time = std::chrono::system_clock::now() -
                      std::chrono::seconds(10);
  scheduler.Schedule(std::move(item));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  }, 1000);

  scheduler.Stop();

  ASSERT_TRUE(got) << "Past-due item should fire immediately";
}

TEST(SchedulerDeviceTest, StopWithoutStartDoesNotCrash) {
  SchedulerDevice scheduler;
  scheduler.SetOutputCallback([](const std::string&, const std::string&,
                                 io::DataFrame) {});
  // Stop without Start — should be a no-op.
  EXPECT_NO_THROW(scheduler.Stop());
}

}  // namespace
}  // namespace shizuru::app
