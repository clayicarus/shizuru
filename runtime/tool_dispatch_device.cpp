#include "runtime/tool_dispatch_device.h"

#include <chrono>
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
      {"control_in", io::PortDirection::kInput, "",
       runtime::PortPayloadKind::kControlSignal},
      {kActionIn,  io::PortDirection::kInput,  "action/tool_call"},
      {kResultOut, io::PortDirection::kOutput, "action/tool_result"},
  };
}

void ToolDispatchDevice::OnInput(const std::string& port_name,
                                 io::DataFrame frame) {
  if (port_name != kActionIn) { return; }
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    task_queue_.push(std::move(frame));
  }
  worker_cv_.notify_one();
}

void ToolDispatchDevice::OnControlSignal(const std::string& port_name,
                                         core::ControlSignal signal) {
  (void)port_name;
  if (auto* tcs = std::get_if<core::ToolCallStartSignal>(&signal)) {
    LOG_INFO("ToolDispatchDevice: control_in received tool_call name='{}' id='{}'",
             tcs->name, tcs->tool_call_id);
    auto args = nlohmann::json::parse(tcs->arguments, nullptr, false);
    if (args.is_discarded()) {
      args = nlohmann::json::object();
    }
    nlohmann::json request = {
        {"tool_call_id", tcs->tool_call_id},
        {"tool_name", tcs->name},
        {"arguments", std::move(args)},
    };
    io::DataFrame frame;
    frame.type = "action/tool_call";
    const auto payload = request.dump();
    frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());
    frame.metadata["tool_call_id"] = tcs->tool_call_id;
    frame.metadata["tool_name"] = tcs->name;
    frame.timestamp = std::chrono::steady_clock::now();
    OnInput(kActionIn, std::move(frame));
  }
}

void ToolDispatchDevice::SetOutputCallback(io::OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void ToolDispatchDevice::SetOnResultCallback(ToolResultCallback cb) {
  std::lock_guard<std::mutex> lock(on_result_mutex_);
  on_result_ = std::move(cb);
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
      io::DataFrame frame = std::move(task_queue_.front());
      task_queue_.pop();
      lock.unlock();
      Dispatch(std::move(frame));
      lock.lock();
    }

    if (worker_stop_.load()) { break; }
  }
}

void ToolDispatchDevice::Dispatch(io::DataFrame frame) {
  const std::string payload(frame.payload.begin(), frame.payload.end());

  std::string tool_call_id;
  std::string tool_name;
  nlohmann::json arguments = nlohmann::json::object();

  try {
    const auto request = nlohmann::json::parse(payload);
    tool_call_id = request.value("tool_call_id", "");
    tool_name = request.value("tool_name", "");
    if (request.contains("arguments")) {
      arguments = request["arguments"];
    }
  } catch (const std::exception& e) {
    nlohmann::json error = {
        {"success", false},
        {"error", std::string("Malformed tool call payload: ") + e.what()},
    };
    if (!tool_call_id.empty()) {
      error["tool_call_id"] = tool_call_id;
    }
    io::DataFrame result_frame;
    result_frame.type = "action/tool_result";
    const auto body = error.dump();
    result_frame.payload = std::vector<uint8_t>(body.begin(), body.end());
    result_frame.source_device = device_id_;
    result_frame.source_port = kResultOut;
    result_frame.timestamp = std::chrono::steady_clock::now();

    io::OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, kResultOut, std::move(result_frame)); }
    return;
  }

  if (tool_call_id.empty()) {
    auto it = frame.metadata.find("tool_call_id");
    if (it != frame.metadata.end()) { tool_call_id = it->second; }
  }
  if (tool_name.empty()) {
    auto it = frame.metadata.find("tool_name");
    if (it != frame.metadata.end()) { tool_name = it->second; }
  }

  if (tool_name.empty()) {
    nlohmann::json error = {
        {"success", false},
        {"error", "Missing tool_name"},
    };
    if (!tool_call_id.empty()) {
      error["tool_call_id"] = tool_call_id;
    }
    io::DataFrame result_frame;
    result_frame.type = "action/tool_result";
    const auto body = error.dump();
    result_frame.payload = std::vector<uint8_t>(body.begin(), body.end());
    result_frame.source_device = device_id_;
    result_frame.source_port = kResultOut;
    result_frame.timestamp = std::chrono::steady_clock::now();

    io::OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, kResultOut, std::move(result_frame)); }
    return;
  }

  nlohmann::json result_json;

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

  // Emit ToolResultSignal to Core (new semantic path).
  {
    core::ToolResultSignal signal;
    signal.tool_call_id = tool_call_id;
    signal.content = result_json.dump();
    signal.success = result_json.value("success", false);

    ToolResultCallback result_cb;
    {
      std::lock_guard<std::mutex> lock(on_result_mutex_);
      result_cb = on_result_;
    }
    if (result_cb) {
      LOG_INFO("ToolDispatchDevice: delivering tool result id='{}' success={}",
               signal.tool_call_id, signal.success);
      result_cb(std::move(signal));
    }
  }

  // Legacy DataFrame output (for backward compatibility during migration).
  io::DataFrame result_frame;
  result_frame.type = "action/tool_result";
  const auto result_body = result_json.dump();
  result_frame.payload =
      std::vector<uint8_t>(result_body.begin(), result_body.end());
  result_frame.source_device = device_id_;
  result_frame.source_port = kResultOut;
  result_frame.timestamp = std::chrono::steady_clock::now();

  io::OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (cb) { cb(device_id_, kResultOut, std::move(result_frame)); }
}

}  // namespace shizuru::runtime
