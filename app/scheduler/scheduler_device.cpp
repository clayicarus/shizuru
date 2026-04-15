// app/scheduler/scheduler_device.cpp — Timer-based IoDevice.
//
// Timer loop sleeps until the earliest item's trigger time, then emits a
// DataFrame on event_out.  New Schedule() calls wake the loop so it can
// recalculate the sleep duration.

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
  // Wake the timer thread so it recalculates the next wakeup time.
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

std::vector<io::PortDescriptor> SchedulerDevice::GetPortDescriptors() const {
  return {{kEventOut, io::PortDirection::kOutput, "scheduler/event"}};
}

void SchedulerDevice::OnInput(const std::string& /*port_name*/,
                              io::DataFrame /*frame*/) {
  // SchedulerDevice has no input ports — ignore.
}

void SchedulerDevice::SetOutputCallback(io::OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
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
        now + std::chrono::hours(24);  // default: wake up in 24h
    bool has_pending = false;

    for (const auto& item : items_) {
      if (!item.fired && item.trigger_time < earliest) {
        earliest = item.trigger_time;
        has_pending = true;
      }
    }

    if (!has_pending) {
      // Nothing scheduled — sleep until woken by Schedule() or Stop().
      items_cv_.wait(lock, [this] {
        return stop_.load() || !items_.empty();
      });
      continue;
    }

    if (earliest > now) {
      // Sleep until the earliest trigger time or until woken.
      items_cv_.wait_until(lock, earliest, [this, &earliest] {
        if (stop_.load()) { return true; }
        // Check if a newly scheduled item has an earlier trigger time.
        for (const auto& item : items_) {
          if (!item.fired && item.trigger_time < earliest) {
            return true;  // Wake up to recalculate.
          }
        }
        return false;
      });
      if (stop_.load()) { break; }
      continue;  // Re-enter loop to check what's ready.
    }

    // Fire all items whose trigger time has passed.
    std::vector<ScheduledItem*> ready;
    for (auto& item : items_) {
      if (!item.fired && item.trigger_time <= now) {
        ready.push_back(&item);
      }
    }

    // Release the lock before emitting frames (callback may re-enter).
    lock.unlock();

    for (auto* item : ready) {
      io::DataFrame frame;
      frame.type = "scheduler/event";
      frame.payload = std::vector<uint8_t>(
          item->payload.begin(), item->payload.end());
      frame.source_device = device_id_;
      frame.source_port = kEventOut;
      frame.metadata["scheduler_item_id"] = item->id;
      frame.timestamp = std::chrono::steady_clock::now();

      io::OutputCallback cb;
      {
        std::lock_guard<std::mutex> cb_lock(output_cb_mutex_);
        cb = output_cb_;
      }
      if (cb) { cb(device_id_, kEventOut, std::move(frame)); }

      // Mark as fired (need to re-acquire items lock).
      std::lock_guard<std::mutex> items_lock(items_mutex_);
      item->fired = true;
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
