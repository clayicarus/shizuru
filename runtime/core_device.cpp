#include "core_device.h"

#include <chrono>
#include <functional>
#include <utility>

#include "async_logger.h"

namespace shizuru::runtime {

namespace {

// Wraps an IoBridge to intercept Execute() calls and emit action/tool_call
// DataFrames before delegating to the real bridge.
class InterceptingIoBridge : public core::IoBridge {
 public:
  using EmitFn = std::function<void(const core::ActionCandidate&)>;

  InterceptingIoBridge(std::unique_ptr<core::IoBridge> inner, EmitFn on_execute)
      : inner_(std::move(inner)), on_execute_(std::move(on_execute)) {}

  core::ActionResult Execute(const core::ActionCandidate& action) override {
    if (on_execute_) on_execute_(action);
    return inner_->Execute(action);
  }

  void Cancel() override { inner_->Cancel(); }

 private:
  std::unique_ptr<core::IoBridge> inner_;
  EmitFn on_execute_;
};

}  // namespace



CoreDevice::CoreDevice(std::string device_id,
                       std::string session_id,
                       core::ControllerConfig ctrl_config,
                       core::ContextConfig ctx_config,
                       core::PolicyConfig pol_config,
                       std::unique_ptr<core::LlmClient> llm,
                       std::unique_ptr<core::IoBridge> io,
                       std::unique_ptr<core::MemoryStore> memory,
                       std::unique_ptr<core::AuditSink> audit)
    : device_id_(std::move(device_id)) {
  // Wrap the IoBridge to intercept tool call dispatches and emit DataFrames.
  auto intercepting_io = std::make_unique<InterceptingIoBridge>(
      std::move(io),
      [this](const core::ActionCandidate& action) {
        const std::string serialized =
            action.action_name + ":" + action.arguments;
        io::DataFrame frame;
        frame.type = "action/tool_call";
        frame.payload =
            std::vector<uint8_t>(serialized.begin(), serialized.end());
        frame.source_device = device_id_;
        frame.source_port = kActionOut;
        frame.timestamp = std::chrono::steady_clock::now();
        EmitFrame(kActionOut, std::move(frame));
      });

  session_ = std::make_unique<core::AgentSession>(
      std::move(session_id),
      std::move(ctrl_config),
      std::move(ctx_config),
      std::move(pol_config),
      std::move(llm),
      std::move(intercepting_io),
      std::move(memory),
      std::move(audit));

  // Hook Controller::OnResponse to emit DataFrames for kResponse actions.
  session_->GetController().OnResponse(
      [this](const core::ActionCandidate& action) {
        if (action.type == core::ActionType::kResponse) {
          const std::string& text = action.response_text;
          io::DataFrame frame;
          frame.type = "text/plain";
          frame.payload = std::vector<uint8_t>(text.begin(), text.end());
          frame.source_device = device_id_;
          frame.source_port = kTextOut;
          frame.timestamp = std::chrono::steady_clock::now();
          EmitFrame(kTextOut, std::move(frame));
        }
      });
}

std::string CoreDevice::GetDeviceId() const {
  return device_id_;
}

std::vector<io::PortDescriptor> CoreDevice::GetPortDescriptors() const {
  return {
      {kTextIn,       io::PortDirection::kInput,  "text/plain"},
      {kToolResultIn, io::PortDirection::kInput,  "action/tool_result"},
      {kTextOut,      io::PortDirection::kOutput, "text/plain"},
      {kActionOut,    io::PortDirection::kOutput, "action/tool_call"},
  };
}

void CoreDevice::OnInput(const std::string& port_name, io::DataFrame frame) {
  if (!active_.load()) {
    return;
  }

  if (port_name == kTextIn) {
    const std::string content(frame.payload.begin(), frame.payload.end());
    core::Observation obs;
    obs.type = core::ObservationType::kUserMessage;
    obs.content = content;
    obs.source = "user";
    obs.timestamp = std::chrono::steady_clock::now();
    session_->EnqueueObservation(std::move(obs));
  } else if (port_name == kToolResultIn) {
    const std::string content(frame.payload.begin(), frame.payload.end());
    core::Observation obs;
    obs.type = core::ObservationType::kToolResult;
    obs.content = content;
    obs.source = "tool";
    obs.timestamp = std::chrono::steady_clock::now();
    session_->EnqueueObservation(std::move(obs));
  } else {
    LOG_WARN("CoreDevice: unsupported input port: {}", port_name);
  }
}

void CoreDevice::SetOutputCallback(io::OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void CoreDevice::Start() {
  active_.store(true);
  session_->Start();
}

void CoreDevice::Stop() {
  active_.store(false);
  session_->Shutdown();
}

core::AgentSession& CoreDevice::Session() {
  return *session_;
}

core::State CoreDevice::GetState() const {
  return session_->GetState();
}

void CoreDevice::EmitFrame(const std::string& port_name, io::DataFrame frame) {
  io::OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (cb) {
    cb(device_id_, port_name, std::move(frame));
  }
}

}  // namespace shizuru::runtime
