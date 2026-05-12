#include "core_device.h"

#include <chrono>
#include <functional>
#include <utility>
#include <variant>

#include "async_logger.h"

namespace shizuru::runtime {

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
  // CancelCallback: called by Controller on interrupt.
  auto cancel = [this]() {
    EmitControlSignal(kSignalOut, core::CancelSignal{});
  };

  session_ = std::make_unique<core::AgentSession>(
      std::move(session_id),
      std::move(ctrl_config),
      std::move(ctx_config),
      std::move(pol_config),
      std::move(llm),
      std::move(cancel),
      std::move(history),
      std::move(audit),
      std::move(tts_segment),
      std::move(response_filter));

  session_->GetController().OnTransition(
      [this](core::State /*from*/, core::State to, core::Event event) {
        if (to == core::State::kListening &&
            event == core::Event::kInterrupt) {
          EmitControlSignal(kSignalOut, core::CancelSignal{});
        }
        if (to == core::State::kError) {
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
      {kItemIn,        io::PortDirection::kInput,  "",
                       runtime::PortPayloadKind::kConversationItem},
      {kControlIn,     io::PortDirection::kInput,  "",
                       runtime::PortPayloadKind::kControlSignal},
      {kSignalOut,     io::PortDirection::kOutput, "",
                       runtime::PortPayloadKind::kControlSignal},
      {kItemOut,       io::PortDirection::kOutput, "",
                       runtime::PortPayloadKind::kConversationItem},
  };
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
