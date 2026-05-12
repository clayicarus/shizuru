#include "log_device.h"

#include <algorithm>
#include <variant>
#include <utility>

namespace shizuru::io {

LogDevice::LogDevice(std::string device_id,
                     spdlog::level::level_enum level)
    : device_id_(std::move(device_id)), level_(level) {}

std::string LogDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> LogDevice::GetPortDescriptors() const {
  return {
      {kAudioIn, PortDirection::kInput, "audio/pcm",
       runtime::PortPayloadKind::kAudioFrame},
      {kAudioOut, PortDirection::kOutput, "audio/pcm",
       runtime::PortPayloadKind::kAudioFrame},
      {kItemIn, PortDirection::kInput, "",
       runtime::PortPayloadKind::kConversationItem},
      {kItemOut, PortDirection::kOutput, "",
       runtime::PortPayloadKind::kConversationItem},
      {kSignalIn, PortDirection::kInput, "",
       runtime::PortPayloadKind::kControlSignal},
      {kSignalOut, PortDirection::kOutput, "",
       runtime::PortPayloadKind::kControlSignal},
  };
}

void LogDevice::OnAudioFrame(const std::string& port_name, AudioFrame frame) {
  if (port_name != kAudioIn) { return; }
  core::GetLogger()->log(level_, "[{}] {}", device_id_, FormatAudioFrame(frame));
  if (audio_output_cb_) { audio_output_cb_(device_id_, kAudioOut, std::move(frame)); }
}

void LogDevice::OnConversationItem(const std::string& port_name,
                                   core::ConversationItem item) {
  if (port_name != kItemIn) { return; }
  core::GetLogger()->log(level_, "[{}] {}", device_id_, FormatConversationItem(item));
  if (item_output_cb_) { item_output_cb_(device_id_, kItemOut, std::move(item)); }
}

void LogDevice::OnControlSignal(const std::string& port_name,
                                core::ControlSignal signal) {
  if (port_name != kSignalIn) { return; }
  core::GetLogger()->log(level_, "[{}] {}", device_id_, FormatControlSignal(signal));
  if (signal_output_cb_) { signal_output_cb_(device_id_, kSignalOut, std::move(signal)); }
}

void LogDevice::SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) {
  audio_output_cb_ = std::move(cb);
}

void LogDevice::SetConversationItemOutputCallback(
    ConversationItemOutputCallback cb) {
  item_output_cb_ = std::move(cb);
}

void LogDevice::SetControlSignalOutputCallback(ControlSignalOutputCallback cb) {
  signal_output_cb_ = std::move(cb);
}

void LogDevice::Start() {}
void LogDevice::Stop()  {}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

std::string LogDevice::FormatAudioFrame(const AudioFrame& frame) const {
  return "audio/pcm samples=" + std::to_string(frame.sample_count) +
         " sample_rate=" + std::to_string(frame.sample_rate) +
         " channels=" + std::to_string(frame.channel_count);
}

std::string LogDevice::FormatConversationItem(
    const core::ConversationItem& item) const {
  std::string text;
  for (const auto& part : item.parts) {
    if (const auto* tp = std::get_if<core::TextPart>(&part)) {
      text += tp->text;
    }
  }
  if (text.size() > 120) {
    text.resize(120);
    text += "...";
  }
  return "item kind=" + std::to_string(static_cast<int>(item.kind)) +
         (text.empty() ? "" : " \"" + text + "\"");
}

std::string LogDevice::FormatControlSignal(const core::ControlSignal& signal) const {
  if (std::holds_alternative<core::InterruptSignal>(signal)) {
    return "signal interrupt";
  }
  if (std::holds_alternative<core::FlushSignal>(signal)) {
    return "signal flush";
  }
  if (std::holds_alternative<core::CancelSignal>(signal)) {
    return "signal cancel";
  }
  if (const auto* tool_call = std::get_if<core::ToolCallStartSignal>(&signal)) {
    return "signal tool_call name=" + tool_call->name +
           " id=" + tool_call->tool_call_id;
  }
  if (const auto* tool_result = std::get_if<core::ToolResultSignal>(&signal)) {
    return "signal tool_result id=" + tool_result->tool_call_id +
           " success=" + std::string(tool_result->success ? "true" : "false");
  }
  return "signal unknown";
}

}  // namespace shizuru::io
