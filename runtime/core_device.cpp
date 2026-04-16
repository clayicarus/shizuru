#include "core_device.h"

#include <chrono>
#include <functional>
#include <utility>

#include "async_logger.h"
#include "conversation/item.h"
#include "io/control_frame.h"

namespace shizuru::runtime {

CoreDevice::CoreDevice(std::string device_id,
                       std::string session_id,
                       core::ControllerConfig ctrl_config,
                       core::ContextConfig ctx_config,
                       core::PolicyConfig pol_config,
                       std::unique_ptr<core::LlmClient> llm,
                       std::unique_ptr<core::MemoryStore> memory,
                       std::unique_ptr<core::AuditSink> audit,
                       std::unique_ptr<core::ObservationAggregator> observation_aggregator,
                       std::unique_ptr<core::ObservationFilter> observation_filter,
                       std::unique_ptr<core::TtsSegmentStrategy> tts_segment,
                       std::unique_ptr<core::ResponseFilter> response_filter)
    : device_id_(std::move(device_id)) {
  // EmitFrameCallback: called by Controller to emit action/tool_call frames.
  auto emit_frame = [this](const std::string& port, io::DataFrame frame) {
    EmitFrame(port, std::move(frame));
  };

  // CancelCallback: called by Controller on interrupt — emits cancel on control_out.
  auto cancel = [this]() {
    EmitFrame(kControlOut, io::ControlFrame::Make("cancel"));
  };

  session_ = std::make_unique<core::AgentSession>(
      std::move(session_id),
      std::move(ctrl_config),
      std::move(ctx_config),
      std::move(pol_config),
      std::move(llm),
      std::move(emit_frame),
      std::move(cancel),
      std::move(memory),
      std::move(audit),
      std::move(observation_aggregator),
      std::move(observation_filter),
      std::move(tts_segment),
      std::move(response_filter));

  // OnTransition: emit cancel on control_out when transitioning to kListening
  // via kInterrupt or kResponseDelivered.
  session_->GetController().OnTransition(
      [this](core::State /*from*/, core::State to, core::Event event) {
        if (to == core::State::kListening &&
            event == core::Event::kInterrupt) {
          // User interrupted — cancel in-progress TTS/playout immediately.
          EmitFrame(kControlOut, io::ControlFrame::Make("cancel"));
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
      {kVadIn,        io::PortDirection::kInput,  "vad/event"},
      {kSchedulerIn,  io::PortDirection::kInput,  "scheduler/event"},
      {kTtsOut,       io::PortDirection::kOutput, "text/plain"},
      {kActionOut,    io::PortDirection::kOutput, "action/tool_call"},
      {kControlOut,   io::PortDirection::kOutput, "control/command"},
  };
}

void CoreDevice::OnInput(const std::string& port_name, io::DataFrame frame) {
  if (!active_.load()) {
    return;
  }

  if (port_name == kTextIn) {
    const std::string content(frame.payload.begin(), frame.payload.end());
    std::string actor_id = "user";
    if (frame.metadata.count("actor_id") != 0) {
      actor_id = frame.metadata.at("actor_id");
    }
    std::string actor_name;
    if (frame.metadata.count("actor_name") != 0) {
      actor_name = frame.metadata.at("actor_name");
    }
    core::Observation obs;
    obs.type = core::ObservationType::kUserMessage;
    obs.content = content;
    obs.source = actor_id;
    obs.timestamp = std::chrono::steady_clock::now();
    obs.item = core::conversation::MakeHumanMessageItem(
        std::move(actor_id), std::move(actor_name), content);
    session_->EnqueueObservation(std::move(obs));
  } else if (port_name == kToolResultIn) {
    const std::string content(frame.payload.begin(), frame.payload.end());
    core::Observation obs;
    obs.type = core::ObservationType::kToolResult;
    obs.content = content;
    obs.source = "tool";
    obs.timestamp = std::chrono::steady_clock::now();
    auto json = core::conversation::ParseJsonOrString(content);
    if (json.is_object()) {
      const std::string tool_name = json.value("tool_name", "tool");
      const std::string tool_call_id = json.value("tool_call_id", "");
      obs.item = core::conversation::MakeToolResultItem(
          tool_name, tool_call_id, std::move(json));
      obs.source = "tool:" + tool_name;
    }
    session_->EnqueueObservation(std::move(obs));
  } else if (port_name == kVadIn) {
    const std::string event_name(frame.payload.begin(), frame.payload.end());
    LOG_INFO("CoreDevice: vad_in received event '{}'", event_name);
    if (event_name == "speech_end") {
      LOG_INFO("CoreDevice: emitting flush on control_out");
      EmitFrame(kControlOut, io::ControlFrame::Make("flush"));
    } else if (event_name == "speech_start") {
      // User started speaking — cancel TTS/playout immediately via control_out,
      // then interrupt the controller so it transitions out of kThinking/kActing.
      EmitFrame(kControlOut, io::ControlFrame::Make("cancel"));
      session_->GetController().Interrupt();
    }
    // speech_active is silently ignored.
  } else if (port_name == kSchedulerIn) {
    // Scheduler events bypass aggregator and filter.
    const std::string content(frame.payload.begin(), frame.payload.end());
    std::string event_type = "reminder";
    if (frame.metadata.count("event_type") != 0) {
      event_type = frame.metadata.at("event_type");
    }
    core::Observation obs;
    obs.type = core::ObservationType::kSystemEvent;
    obs.content = content;
    obs.source = "scheduler";
    obs.timestamp = std::chrono::steady_clock::now();
    obs.item = core::conversation::MakeSystemEventItem(
        "system:scheduler", "Scheduler", std::move(event_type), "scheduler",
        core::conversation::ParseJsonOrString(content));
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
