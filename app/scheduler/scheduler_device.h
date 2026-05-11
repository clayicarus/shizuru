#pragma once

// app/scheduler/scheduler_device.h — Timer-based device for reminders and followups.
//
// Runs a background timer thread.  When a scheduled item's trigger time
// arrives, constructs a ConversationItem(kSystemEvent) and delivers it via
// the on_item_ callback.
//
// Supports:
//   - One-shot reminders (fire once at a specific time)
//   - Periodic check-ins (fire at intervals, e.g., daily followup scan)
//
// The SchedulerDevice does NOT make semantic decisions — it only fires
// time-based events.  The Controller + persona prompt decide what to say.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/conversation_item.h"

namespace shizuru::app {

struct ScheduledItem {
  std::string id;
  std::string payload;  // Opaque string (e.g., reminder text or JSON).
  std::chrono::system_clock::time_point trigger_time;
  bool fired = false;
};

// Callback type for delivering scheduler events as ConversationItems.
using SchedulerItemCallback = std::function<void(core::ConversationItem)>;

class SchedulerDevice {
 public:
  explicit SchedulerDevice(std::string device_id = "scheduler");
  ~SchedulerDevice();

  SchedulerDevice(const SchedulerDevice&) = delete;
  SchedulerDevice& operator=(const SchedulerDevice&) = delete;

  // Schedule a one-shot event.
  void Schedule(ScheduledItem item);

  // Cancel a scheduled item by ID.  Returns true if found and cancelled.
  bool Cancel(const std::string& id);

  // Register the callback that delivers ConversationItems to Core.
  void SetOnItemCallback(SchedulerItemCallback cb);

  // Lifecycle.
  void Start();
  void Stop();

  std::string GetDeviceId() const;

  static constexpr char kEventOut[] = "event_out";

 private:
  void TimerLoop();

  std::string device_id_;

  std::mutex on_item_mutex_;
  SchedulerItemCallback on_item_;

  std::mutex items_mutex_;
  std::condition_variable items_cv_;
  std::vector<ScheduledItem> items_;

  std::thread timer_thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace shizuru::app
