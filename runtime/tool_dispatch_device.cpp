#include "tool_dispatch_device.h"

#include <chrono>
#include <string>
#include <utility>

#include "io/data_frame.h"

namespace shizuru::runtime {

ToolDispatchDevice::ToolDispatchDevice(services::ToolRegistry& registry,
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
      {kActionIn,  io::PortDirection::kInput,  "action/tool_call"},
      {kResultOut, io::PortDirection::kOutput, "action/tool_result"},
  };
}

void ToolDispatchDevice::OnInput(const std::string& port_name,
                                 io::DataFrame frame) {
  if (port_name != kActionIn) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    task_queue_.push(std::move(frame));
  }
  worker_cv_.notify_one();
}

void ToolDispatchDevice::SetOutputCallback(io::OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void ToolDispatchDevice::Start() {
  worker_stop_.store(false);
  worker_thread_ = std::thread(&ToolDispatchDevice::WorkerLoop, this);
}

void ToolDispatchDevice::Stop() {
  // Signal the worker to stop after draining.
  worker_stop_.store(true);
  worker_cv_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void ToolDispatchDevice::WorkerLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_cv_.wait(lock, [this] {
      return !task_queue_.empty() || worker_stop_.load();
    });

    // Drain the queue before checking stop — ensures in-flight calls complete.
    while (!task_queue_.empty()) {
      io::DataFrame frame = std::move(task_queue_.front());
      task_queue_.pop();
      lock.unlock();
      Dispatch(std::move(frame));
      lock.lock();
    }

    if (worker_stop_.load()) {
      break;
    }
  }
}

void ToolDispatchDevice::Dispatch(io::DataFrame frame) {
  // Parse payload: "<tool_name>:<arguments>"
  const std::string payload(frame.payload.begin(), frame.payload.end());
  const auto colon_pos = payload.find(':');
  const std::string tool_name =
      (colon_pos == std::string::npos) ? payload : payload.substr(0, colon_pos);
  const std::string arguments =
      (colon_pos == std::string::npos) ? "" : payload.substr(colon_pos + 1);

  // Extract tool_call_id from frame metadata (set by Controller for parallel calls).
  std::string tool_call_id;
  auto it = frame.metadata.find("tool_call_id");
  if (it != frame.metadata.end()) {
    tool_call_id = it->second;
  }

  std::string result_json;

  try {
    const auto* fn = registry_.Find(tool_name);
    if (!fn) {
      result_json = R"({"success":false,"error":"Unknown tool: )" + tool_name + R"("})";
    } else {
      const services::ToolResult result = (*fn)(arguments);
      if (result.success) {
        std::string escaped;
        escaped.reserve(result.output.size());
        for (char c : result.output) {
          if (c == '"') escaped += "\\\"";
          else if (c == '\\') escaped += "\\\\";
          else escaped += c;
        }
        result_json = R"({"success":true,"output":")" + escaped + R"("})";
      } else {
        std::string escaped;
        escaped.reserve(result.error_message.size());
        for (char c : result.error_message) {
          if (c == '"') escaped += "\\\"";
          else if (c == '\\') escaped += "\\\\";
          else escaped += c;
        }
        result_json = R"({"success":false,"error":")" + escaped + R"("})";
      }
    }
  } catch (const std::exception& e) {
    std::string msg = e.what();
    std::string escaped;
    for (char c : msg) {
      if (c == '"') escaped += "\\\"";
      else if (c == '\\') escaped += "\\\\";
      else escaped += c;
    }
    result_json = R"({"success":false,"error":")" + escaped + R"("})";
  } catch (...) {
    result_json = R"({"success":false,"error":"unknown exception"})";
  }

  // Inject tool_call_id into the result JSON for pairing in Controller.
  if (!tool_call_id.empty()) {
    // Insert before the closing brace.
    result_json.pop_back();  // remove '}'
    result_json += R"(,"tool_call_id":")" + tool_call_id + R"("})";
  }

  io::DataFrame result_frame;
  result_frame.type = "action/tool_result";
  result_frame.payload =
      std::vector<uint8_t>(result_json.begin(), result_json.end());
  result_frame.source_device = device_id_;
  result_frame.source_port = kResultOut;
  result_frame.timestamp = std::chrono::steady_clock::now();

  io::OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (cb) {
    cb(device_id_, kResultOut, std::move(result_frame));
  }
}

}  // namespace shizuru::runtime
