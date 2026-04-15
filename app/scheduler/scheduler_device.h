#pragma once

// app/scheduler/scheduler_device.h — Timer-based IoDevice for reminders and followups.
//
// Runs a background timer thread.  When a scheduled item's trigger time
// arrives, emits a DataFrame on "event_out" that gets routed to CoreDevice,
// causing the Controller to initiate a proactive conversation turn.
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

#include "io/io_device.h"

namespace shizuru::app {

struct ScheduledItem {
  std::string id;
  std::string payload;  // Opaque string passed in the DataFrame (e.g., reminder JSON).
  std::chrono::system_clock::time_point trigger_time;
  bool fired = false;
};

class SchedulerDevice : public io::IoDevice {
 public:
  explicit SchedulerDevice(std::string device_id = "scheduler");
  ~SchedulerDevice() override;

  // Schedule a one-shot event.
  void Schedule(ScheduledItem item);

  // Cancel a scheduled item by ID.  Returns true if found and cancelled.
  bool Cancel(const std::string& id);

  // IoDevice interface.
  std::string GetDeviceId() const override;
  std::vector<io::PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, io::DataFrame frame) override;
  void SetOutputCallback(io::OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kEventOut[] = "event_out";

 private:
  void TimerLoop();

  std::string device_id_;
  io::OutputCallback output_cb_;
  std::mutex output_cb_mutex_;

  std::mutex items_mutex_;
  std::condition_variable items_cv_;
  std::vector<ScheduledItem> items_;

  std::thread timer_thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace shizuru::app
