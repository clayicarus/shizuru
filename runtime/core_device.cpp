#include "core_device.h"

#include <chrono>
#include <functional>
#include <utility>
#include <variant>

#include "async_logger.h"

namespace shizuru::runtime {

io::DataFrame CoreDevice::MakeControlFrame(const std::string& command) {
  io::DataFrame frame;
  frame.type = "control/command";
  frame.payload = std::vector<uint8_t>(command.begin(), command.end());
  frame.timestamp = std::chrono::steady_clock::now();
  return frame;
}

CoreDevice::CoreDevice(std::string device_id,
                       std::string session_id,
                       core::ControllerConfig ctrl_config,
                       core::ContextConfig ctx_config,
                       core::PolicyConfig pol_config,
                       std::unique_ptr<core::LlmClient> llm,
                       std::unique_ptr<core::HistoryStore> history,
                       std::unique_ptr<core::AuditSink> audit,
                       std::unique_ptr<core::TtsSegmentStrategy> tts_segment,
                       std::unique_ptr<core::ResponseFilter> response_filter)
    : device_id_(std::move(device_id)) {
  // EmitFrameCallback: called by Controller to emit action/tool_call frames.
  auto emit_frame = [this](const std::string& port, io::DataFrame frame) {
    EmitFrame(port, std::move(frame));
  };

  // CancelCallback: called by Controller on interrupt.
  auto cancel = [this]() {
    EmitFrame(kControlOut, MakeControlFrame("cancel"));
    EmitControlSignal(kSignalOut, core::CancelSignal{});
  };

  session_ = std::make_unique<core::AgentSession>(
      std::move(session_id),
      std::move(ctrl_config),
      std::move(ctx_config),
      std::move(pol_config),
      std::move(llm),
      std::move(emit_frame),
      std::move(cancel),
      std::move(history),
      std::move(audit),
      std::move(tts_segment),
      std::move(response_filter));

  // OnTransition: emit cancel on control_out when transitioning to kListening
  // via kInterrupt.
  session_->GetController().OnTransition(
      [this](core::State /*from*/, core::State to, core::Event event) {
        if (to == core::State::kListening &&
            event == core::Event::kInterrupt) {
          EmitFrame(kControlOut, MakeControlFrame("cancel"));
          EmitControlSignal(kSignalOut, core::CancelSignal{});
        }
        if (to == core::State::kError) {
          io::DataFrame frame;
          frame.type = "text/plain";
          frame.payload = {'E', 'r', 'r', 'o', 'r'};
          frame.metadata["error"] = "llm_failure";
          frame.timestamp = std::chrono::steady_clock::now();
          EmitFrame(kErrorOut, std::move(frame));

          session_->GetController().Recover();
        }
      });

  // Mirror semantic conversation output onto a typed bus output port so the
  // runtime can observe/replay semantic items without reparsing text frames.
  session_->GetController().OnConversationItem(
      [this](const core::ConversationItem& item, bool /*is_delta*/) {
        EmitConversationItem(kItemOut, item);

        if (item.kind == core::ConversationItemKind::kToolCall) {
          for (const auto& part : item.parts) {
            if (const auto* tcp = std::get_if<core::ToolCallPart>(&part)) {
              EmitControlSignal(
                  kSignalOut,
                  core::ToolCallStartSignal{
                      tcp->tool_call_id,
                      tcp->name,
                      tcp->arguments_json});
            }
          }
        }
      });
}

std::string CoreDevice::GetDeviceId() const {
  return device_id_;
}

std::vector<io::PortDescriptor> CoreDevice::GetPortDescriptors() const {
  return {
      {kTextIn,        io::PortDirection::kInput,  "text/plain"},
      {kToolResultIn,  io::PortDirection::kInput,  "action/tool_result"},
      {kInterruptIn,   io::PortDirection::kInput,  "control/interrupt"},
      {kSchedulerIn,   io::PortDirection::kInput,  "scheduler/event"},
      {"item_in",      io::PortDirection::kInput,  "",
                       runtime::PortPayloadKind::kConversationItem},
      {"control_in",   io::PortDirection::kInput,  "",
                       runtime::PortPayloadKind::kControlSignal},
      {kTtsOut,        io::PortDirection::kOutput, "text/plain"},
      {kActionOut,     io::PortDirection::kOutput, "action/tool_call"},
      {kControlOut,    io::PortDirection::kOutput, "control/command"},
      {kSignalOut,     io::PortDirection::kOutput, "",
                       runtime::PortPayloadKind::kControlSignal},
      {kErrorOut,      io::PortDirection::kOutput, "text/plain"},
      {kItemOut,       io::PortDirection::kOutput, "",
                       runtime::PortPayloadKind::kConversationItem},
  };
}

void CoreDevice::OnInput(const std::string& port_name, io::DataFrame frame) {
  // Legacy input interface — stubbed.
  (void)port_name;
  (void)frame;
}

void CoreDevice::OnConversationItem(const std::string& port_name,
                                    core::ConversationItem item) {
  (void)port_name;
  OnConversationItem(std::move(item));
}

void CoreDevice::OnControlSignal(const std::string& port_name,
                                 core::ControlSignal signal) {
  (void)port_name;
  OnControl(std::move(signal));
}

void CoreDevice::OnConversationItem(core::ConversationItem item) {
  if (!active_.load()) { return; }
  session_->EnqueueItem(std::move(item));
}

void CoreDevice::OnControl(core::ControlSignal signal) {
  if (!active_.load()) { return; }

  if (std::holds_alternative<core::InterruptSignal>(signal)) {
    session_->GetController().Interrupt();
  } else if (std::holds_alternative<core::ToolResultSignal>(signal)) {
    const auto& result = std::get<core::ToolResultSignal>(signal);

    core::ConversationItem item;
    item.item_id = "toolresult:" + result.tool_call_id;
    item.conversation_id = "";
    item.kind = core::ConversationItemKind::kToolResult;
    item.actor = core::ActorRef{"tool", "Tool", core::ActorKind::kTool};
    item.parts.emplace_back(core::ToolResultPart{
        result.tool_call_id, "", result.success, result.content});
    item.wall_time = std::chrono::system_clock::now();

    session_->EnqueueToolResult(std::move(item));
  }
}

void CoreDevice::SetOutputCallback(io::OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void CoreDevice::SetConversationItemOutputCallback(
    io::ConversationItemOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  item_output_cb_ = std::move(cb);
}

void CoreDevice::SetControlSignalOutputCallback(
    io::ControlSignalOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  signal_output_cb_ = std::move(cb);
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

void CoreDevice::EmitConversationItem(const std::string& port_name,
                                      core::ConversationItem item) {
  io::ConversationItemOutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = item_output_cb_;
  }
  if (cb) {
    cb(device_id_, port_name, std::move(item));
  }
}

void CoreDevice::EmitControlSignal(const std::string& port_name,
                                   core::ControlSignal signal) {
  io::ControlSignalOutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = signal_output_cb_;
  }
  if (cb) {
    cb(device_id_, port_name, std::move(signal));
  }
}

}  // namespace shizuru::runtime
