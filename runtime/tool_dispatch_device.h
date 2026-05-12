#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "core/control_signal.h"
#include "io/io_device.h"
#include "runtime/tool_registry.h"

namespace shizuru::runtime {

// IoDevice that executes tool calls on a worker thread and emits results.
// After execution, emits a ToolResultSignal via the typed runtime bus.
// Tool Executor does NOT construct ConversationItem — that is Core's job
// (requirement 9.1, 9.2).
class ToolDispatchDevice : public io::IoDevice {
 public:
  explicit ToolDispatchDevice(ToolRegistry& registry,
                              std::string device_id = "tool_dispatch");
  ~ToolDispatchDevice() override;

  std::string GetDeviceId() const override;
  std::vector<io::PortDescriptor> GetPortDescriptors() const override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void SetControlSignalOutputCallback(
      io::ControlSignalOutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kControlIn[] = "control_in";
  static constexpr char kSignalOut[] = "signal_out";

 private:
  void WorkerLoop();
  void Dispatch(core::ToolCallStartSignal signal);
  void EmitToolResult(core::ToolResultSignal signal);

  std::string device_id_;
  ToolRegistry& registry_;

  io::ControlSignalOutputCallback signal_output_cb_;
  mutable std::mutex output_cb_mutex_;

  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::queue<core::ToolCallStartSignal> task_queue_;
  std::thread worker_thread_;
  std::atomic<bool> worker_stop_{false};
};

}  // namespace shizuru::runtime
