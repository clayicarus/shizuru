#include "runtime/tool_dispatch_device.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "async_logger.h"

namespace shizuru::runtime {

ToolDispatchDevice::ToolDispatchDevice(ToolRegistry& registry,
                                       std::string device_id)
    : device_id_(std::move(device_id)), registry_(registry) {}

ToolDispatchDevice::~ToolDispatchDevice() {
  Stop();
}

std::string ToolDispatchDevice::GetDeviceId() const {
  return device_id_;
}

std::vector<io::PortDescriptor> ToolDispatchDevice::GetPortDescriptors() const {
  return {
      {kControlIn, io::PortDirection::kInput, "",
       runtime::PortPayloadKind::kControlSignal},
      {kSignalOut, io::PortDirection::kOutput, "",
       runtime::PortPayloadKind::kControlSignal},
  };
}

void ToolDispatchDevice::OnControlSignal(const std::string& port_name,
                                         core::ControlSignal signal) {
  (void)port_name;
  if (auto* tcs = std::get_if<core::ToolCallStartSignal>(&signal)) {
    LOG_INFO("ToolDispatchDevice: control_in received tool_call name='{}' id='{}'",
             tcs->name, tcs->tool_call_id);
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      task_queue_.push(*tcs);
    }
    worker_cv_.notify_one();
  }
}

void ToolDispatchDevice::SetControlSignalOutputCallback(
    io::ControlSignalOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  signal_output_cb_ = std::move(cb);
}

void ToolDispatchDevice::Start() {
  worker_stop_.store(false);
  worker_thread_ = std::thread(&ToolDispatchDevice::WorkerLoop, this);
}

void ToolDispatchDevice::Stop() {
  worker_stop_.store(true);
  worker_cv_.notify_all();
  if (worker_thread_.joinable()) { worker_thread_.join(); }
}

void ToolDispatchDevice::WorkerLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_cv_.wait(lock, [this] {
      return !task_queue_.empty() || worker_stop_.load();
    });

    while (!task_queue_.empty()) {
      core::ToolCallStartSignal signal = std::move(task_queue_.front());
      task_queue_.pop();
      lock.unlock();
      Dispatch(std::move(signal));
      lock.lock();
    }

    if (worker_stop_.load()) { break; }
  }
}

void ToolDispatchDevice::Dispatch(core::ToolCallStartSignal signal) {
  const std::string tool_call_id = std::move(signal.tool_call_id);
  const std::string tool_name = std::move(signal.name);
  nlohmann::json result_json;
  nlohmann::json arguments = nlohmann::json::object();

  if (!signal.arguments.empty()) {
    arguments = nlohmann::json::parse(signal.arguments, nullptr, false);
    if (arguments.is_discarded()) {
      result_json = {
          {"success", false},
          {"tool_name", tool_name},
          {"tool_call_id", tool_call_id},
          {"error", "Malformed tool call arguments JSON"},
      };
      EmitToolResult(core::ToolResultSignal{
          tool_call_id,
          result_json.dump(),
          false,
      });
      return;
    }
  }

  if (tool_name.empty()) {
    result_json = {
        {"success", false},
        {"tool_call_id", tool_call_id},
        {"error", "Missing tool_name"},
    };
    EmitToolResult(core::ToolResultSignal{
        tool_call_id,
        result_json.dump(),
        false,
    });
    return;
  }

  try {
    const auto* fn = registry_.Find(tool_name);
    if (fn == nullptr) {
      result_json = {
          {"success", false},
          {"tool_name", tool_name},
          {"error", "Unknown tool: " + tool_name},
      };
    } else {
      const ToolResult result = (*fn)(arguments);
      if (result.success) {
        result_json["success"] = true;
        result_json["tool_name"] = tool_name;
        result_json["output"] = result.output;
      } else {
        result_json["success"] = false;
        result_json["tool_name"] = tool_name;
        result_json["error"] = result.error_message;
      }
    }
  } catch (const std::exception& e) {
    result_json = {
        {"success", false},
        {"tool_name", tool_name},
        {"error", e.what()},
    };
  } catch (...) {
    result_json = {
        {"success", false},
        {"tool_name", tool_name},
        {"error", "unknown exception"},
    };
  }

  if (!tool_call_id.empty()) {
    result_json["tool_call_id"] = tool_call_id;
  }

  EmitToolResult(core::ToolResultSignal{
      tool_call_id,
      result_json.dump(),
      result_json.value("success", false),
  });
}

void ToolDispatchDevice::EmitToolResult(core::ToolResultSignal signal) {
  io::ControlSignalOutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = signal_output_cb_;
  }
  if (!cb) {
    LOG_WARN("ToolDispatchDevice: dropping tool result id='{}' because no signal output callback is registered",
             signal.tool_call_id);
    return;
  }
  LOG_INFO("ToolDispatchDevice: delivering tool result id='{}' success={}",
           signal.tool_call_id, signal.success);
  cb(device_id_, kSignalOut, std::move(signal));
}

}  // namespace shizuru::runtime
