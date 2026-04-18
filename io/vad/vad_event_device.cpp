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
      {kInterruptOut,  PortDirection::kOutput, InterruptFrame::kType},
      {kControlOut,    PortDirection::kOutput, "control/command"},
  };
}

void VadEventDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (port_name != kVadIn) { return; }

  const std::string event(frame.payload.begin(), frame.payload.end());
  LOG_INFO("VadEventDevice: received event '{}'", event);

  if (!output_cb_) {
    LOG_WARN("VadEventDevice: no output_cb, dropping event '{}'", event);
    return;
  }

  EmitRawVadEvent(frame);

  if (event.find("speech_start") != std::string::npos) {
    EmitInterrupt();
  } else if (event.find("speech_end") != std::string::npos) {
    EmitControl(ControlFrame::kCommandFlush);
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
  auto out = InterruptFrame::Make(InterruptFrame::kReasonBargeIn, "voice");
  out.source_device = device_id_;
  out.source_port   = kInterruptOut;
  output_cb_(device_id_, kInterruptOut, std::move(out));
  LOG_DEBUG("VadEventDevice: emitted interrupt on {}", kInterruptOut);
}

void VadEventDevice::EmitControl(std::string_view command) {
  auto out = ControlFrame::Make(command);
  out.source_device = device_id_;
  out.source_port   = kControlOut;
  output_cb_(device_id_, kControlOut, std::move(out));
  LOG_DEBUG("VadEventDevice: emitted control '{}' on {}", command, kControlOut);
}

void VadEventDevice::SetOutputCallback(OutputCallback cb) {
  output_cb_ = std::move(cb);
}

void VadEventDevice::Start() {}
void VadEventDevice::Stop() {}

}  // namespace shizuru::io
