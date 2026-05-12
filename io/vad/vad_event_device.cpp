#include "vad_event_device.h"

#include <utility>

namespace shizuru::io {

VadEventDevice::VadEventDevice(std::string device_id)
    : device_id_(std::move(device_id)) {}

std::string VadEventDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> VadEventDevice::GetPortDescriptors() const {
  return {
      {kSignalIn, PortDirection::kInput, "",
       runtime::PortPayloadKind::kControlSignal},
      {kSignalOut, PortDirection::kOutput, "",
       runtime::PortPayloadKind::kControlSignal},
  };
}

void VadEventDevice::OnControlSignal(const std::string& port_name,
                                     core::ControlSignal signal) {
  if (port_name != kSignalIn) { return; }
  auto cb = signal_output_cb_;
  if (cb) { cb(device_id_, kSignalOut, std::move(signal)); }
}

void VadEventDevice::SetControlSignalOutputCallback(
    ControlSignalOutputCallback cb) {
  signal_output_cb_ = std::move(cb);
}

void VadEventDevice::Start() {}
void VadEventDevice::Stop() {}

}  // namespace shizuru::io
