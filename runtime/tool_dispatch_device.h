#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "io/io_device.h"
#include "runtime/tool_registry.h"

namespace shizuru::runtime {

// IoDevice that executes tool calls on a worker thread and emits results.
// Receives action/tool_call frames on action_in, emits action/tool_result
// frames on result_out.
//
// Conceptually a DMA controller: the CPU (Controller) issues an IO request
// (tool call), the DMA controller dispatches it to the appropriate handler
// (tool function), and signals completion via interrupt (result frame).
class ToolDispatchDevice : public io::IoDevice {
 public:
  explicit ToolDispatchDevice(ToolRegistry& registry,
                              std::string device_id = "tool_dispatch");
  ~ToolDispatchDevice() override;

  std::string GetDeviceId() const override;
  std::vector<io::PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, io::DataFrame frame) override;
  void SetOutputCallback(io::OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kActionIn[]  = "action_in";
  static constexpr char kResultOut[] = "result_out";

 private:
  void WorkerLoop();
  void Dispatch(io::DataFrame frame);

  std::string device_id_;
  ToolRegistry& registry_;

  io::OutputCallback output_cb_;
  mutable std::mutex output_cb_mutex_;

  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::queue<io::DataFrame> task_queue_;
  std::thread worker_thread_;
  std::atomic<bool> worker_stop_{false};
};

}  // namespace shizuru::runtime
