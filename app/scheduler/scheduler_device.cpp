// app/scheduler/scheduler_device.cpp — Timer-based scheduler device.
//
// Timer loop sleeps until the earliest item's trigger time, then constructs
// a ConversationItem(kSystemEvent) and delivers it via the on_item_ callback.

#include "app/scheduler/scheduler_device.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace shizuru::app {

SchedulerDevice::SchedulerDevice(std::string device_id)
    : device_id_(std::move(device_id)) {}

SchedulerDevice::~SchedulerDevice() { Stop(); }

void SchedulerDevice::Schedule(ScheduledItem item) {
  {
    std::lock_guard<std::mutex> lock(items_mutex_);
    items_.push_back(std::move(item));
  }
  items_cv_.notify_one();
}

bool SchedulerDevice::Cancel(const std::string& id) {
  std::lock_guard<std::mutex> lock(items_mutex_);
  auto it = std::find_if(items_.begin(), items_.end(),
                         [&](const ScheduledItem& item) {
                           return item.id == id && !item.fired;
                         });
  if (it == items_.end()) { return false; }
  items_.erase(it);
  return true;
}

std::string SchedulerDevice::GetDeviceId() const { return device_id_; }

void SchedulerDevice::SetOnItemCallback(SchedulerItemCallback cb) {
  std::lock_guard<std::mutex> lock(on_item_mutex_);
  on_item_ = std::move(cb);
}

void SchedulerDevice::Start() {
  stop_.store(false);
  timer_thread_ = std::thread(&SchedulerDevice::TimerLoop, this);
}

void SchedulerDevice::Stop() {
  stop_.store(true);
  items_cv_.notify_all();
  if (timer_thread_.joinable()) { timer_thread_.join(); }
}

void SchedulerDevice::TimerLoop() {
  while (!stop_.load()) {
    std::unique_lock<std::mutex> lock(items_mutex_);

    // Find the earliest unfired item.
    auto now = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point earliest =
        now + std::chrono::hours(24);
    bool has_pending = false;

    for (const auto& item : items_) {
      if (!item.fired && item.trigger_time < earliest) {
        earliest = item.trigger_time;
        has_pending = true;
      }
    }

    if (!has_pending) {
      items_cv_.wait(lock, [this] {
        return stop_.load() || !items_.empty();
      });
      continue;
    }

    if (earliest > now) {
      items_cv_.wait_until(lock, earliest, [this, &earliest] {
        if (stop_.load()) { return true; }
        for (const auto& item : items_) {
          if (!item.fired && item.trigger_time < earliest) {
            return true;
          }
        }
        return false;
      });
      if (stop_.load()) { break; }
      continue;
    }

    // Fire all items whose trigger time has passed.
    std::vector<ScheduledItem*> ready;
    for (auto& item : items_) {
      if (!item.fired && item.trigger_time <= now) {
        ready.push_back(&item);
      }
    }

    // Release the lock before delivering items (callback may re-enter).
    lock.unlock();

    for (auto* sched_item : ready) {
      // Construct a ConversationItem(kSystemEvent) from the scheduled item.
      core::ConversationItem item;
      item.item_id = "scheduler:" + sched_item->id;
      item.conversation_id = "";  // Core will assign the active conversation.
      item.kind = core::ConversationItemKind::kSystemEvent;
      item.actor = core::ActorRef{"scheduler", "Scheduler",
                                  core::ActorKind::kSystem};
      item.parts.emplace_back(core::TextPart{sched_item->payload});
      item.wall_time = std::chrono::system_clock::now();

      SchedulerItemCallback cb;
      {
        std::lock_guard<std::mutex> cb_lock(on_item_mutex_);
        cb = on_item_;
      }
      if (cb) { cb(std::move(item)); }

      // Mark as fired.
      std::lock_guard<std::mutex> items_lock(items_mutex_);
      sched_item->fired = true;
    }

    // Clean up fired items.
    {
      std::lock_guard<std::mutex> items_lock(items_mutex_);
      items_.erase(
          std::remove_if(items_.begin(), items_.end(),
                         [](const ScheduledItem& item) { return item.fired; }),
          items_.end());
    }
  }
}

}  // namespace shizuru::app
