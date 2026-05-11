#include "vad_event_device.h"

#include <chrono>
#include <string_view>
#include <utility>

#include "async_logger.h"

namespace shizuru::io {

VadEventDevice::VadEventDevice(std::string device_id)
    : device_id_(std::move(device_id)) {}

std::string VadEventDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> VadEventDevice::GetPortDescriptors() const {
  return {
      {kVadIn,         PortDirection::kInput,  "vad/event"},
      {kVadOut,        PortDirection::kOutput, "vad/event"},
      {kInterruptOut,  PortDirection::kOutput, "control/interrupt"},
      {kControlOut,    PortDirection::kOutput, "control/command"},
      {kInterruptSignalOut, PortDirection::kOutput, "",
       runtime::PortPayloadKind::kControlSignal},
      {kControlSignalOut, PortDirection::kOutput, "",
       runtime::PortPayloadKind::kControlSignal},
  };
}

void VadEventDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (port_name != kVadIn) { return; }

  const std::string event(frame.payload.begin(), frame.payload.end());
  LOG_INFO("VadEventDevice: received event '{}'", event);

  if (!output_cb_ && !signal_output_cb_) {
    LOG_WARN("VadEventDevice: no output_cb, dropping event '{}'", event);
    return;
  }

  if (output_cb_) {
    EmitRawVadEvent(frame);
  }

  if (event.find("speech_start") != std::string::npos) {
    EmitInterrupt();
    EmitInterruptSignal();
  } else if (event.find("speech_end") != std::string::npos) {
    EmitControl("flush");
    EmitFlushSignal();
  }
}

void VadEventDevice::EmitRawVadEvent(const DataFrame& frame) {
  const std::string event(frame.payload.begin(), frame.payload.end());

  DataFrame out;
  out.type           = "vad/event";
  out.payload        = frame.payload;
  out.source_device  = device_id_;
  out.source_port    = kVadOut;
  out.timestamp      = std::chrono::steady_clock::now();

  output_cb_(device_id_, kVadOut, std::move(out));
  LOG_DEBUG("VadEventDevice: emitted '{}' on vad_out", event);
}

void VadEventDevice::EmitInterrupt() {
  if (!output_cb_) { return; }
  DataFrame out;
  out.type = "control/interrupt";
  std::string payload = "barge_in";
  out.payload = std::vector<uint8_t>(payload.begin(), payload.end());
  out.source_device = device_id_;
  out.source_port   = kInterruptOut;
  out.timestamp     = std::chrono::steady_clock::now();
  out.metadata["reason"] = "barge_in";
  out.metadata["source"] = "voice";
  output_cb_(device_id_, kInterruptOut, std::move(out));
  LOG_DEBUG("VadEventDevice: emitted interrupt on {}", kInterruptOut);
}

void VadEventDevice::EmitControl(std::string_view command) {
  if (!output_cb_) { return; }
  DataFrame out;
  out.type = "control/command";
  out.payload = std::vector<uint8_t>(command.begin(), command.end());
  out.source_device = device_id_;
  out.source_port   = kControlOut;
  out.timestamp     = std::chrono::steady_clock::now();
  output_cb_(device_id_, kControlOut, std::move(out));
  LOG_DEBUG("VadEventDevice: emitted control '{}' on {}", command, kControlOut);
}

void VadEventDevice::EmitInterruptSignal() {
  auto cb = signal_output_cb_;
  if (!cb) { return; }
  cb(device_id_, kInterruptSignalOut, core::InterruptSignal{});
  LOG_DEBUG("VadEventDevice: emitted typed interrupt on {}", kInterruptSignalOut);
}

void VadEventDevice::EmitFlushSignal() {
  auto cb = signal_output_cb_;
  if (!cb) { return; }
  cb(device_id_, kControlSignalOut, core::FlushSignal{});
  LOG_DEBUG("VadEventDevice: emitted typed flush on {}", kControlSignalOut);
}

void VadEventDevice::SetOutputCallback(OutputCallback cb) {
  output_cb_ = std::move(cb);
}

void VadEventDevice::SetControlSignalOutputCallback(
    ControlSignalOutputCallback cb) {
  signal_output_cb_ = std::move(cb);
}

void VadEventDevice::Start() {}
void VadEventDevice::Stop() {}

}  // namespace shizuru::io
