#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "core/control_signal.h"
#include "io/io_device.h"
#include "runtime/tool_registry.h"

namespace shizuru::runtime {

// Callback for delivering ToolResultSignal to Core.
using ToolResultCallback = std::function<void(core::ToolResultSignal)>;

// IoDevice that executes tool calls on a worker thread and emits results.
// After execution, emits a ToolResultSignal via the on_result_ callback.
// Tool Executor does NOT construct ConversationItem — that is Core's job
// (requirement 9.1, 9.2).
class ToolDispatchDevice : public io::IoDevice {
 public:
  explicit ToolDispatchDevice(ToolRegistry& registry,
                              std::string device_id = "tool_dispatch");
  ~ToolDispatchDevice() override;

  std::string GetDeviceId() const override;
  std::vector<io::PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, io::DataFrame frame) override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void SetOutputCallback(io::OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // Register callback for delivering tool results as ControlSignals.
  void SetOnResultCallback(ToolResultCallback cb);

  static constexpr char kActionIn[]  = "action_in";
  static constexpr char kResultOut[] = "result_out";

 private:
  void WorkerLoop();
  void Dispatch(io::DataFrame frame);

  std::string device_id_;
  ToolRegistry& registry_;

  io::OutputCallback output_cb_;
  mutable std::mutex output_cb_mutex_;

  std::mutex on_result_mutex_;
  ToolResultCallback on_result_;

  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::queue<io::DataFrame> task_queue_;
  std::thread worker_thread_;
  std::atomic<bool> worker_stop_{false};
};

}  // namespace shizuru::runtime
