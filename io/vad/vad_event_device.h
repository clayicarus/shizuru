#pragma once

#include "core/control_signal.h"
#include "io/vad/vad_device.h"

namespace shizuru::io {

// Deprecated compatibility adapter kept only for older examples.
// Phase 5 routes VAD directly via typed control signals, so this device simply
// forwards typed control signals unchanged.
class VadEventDevice : public IoDevice {
 public:
  explicit VadEventDevice(std::string device_id = "vad_event");

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void SetControlSignalOutputCallback(ControlSignalOutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kSignalIn[] = "signal_in";
  static constexpr char kSignalOut[] = "signal_out";

 private:
  std::string device_id_;
  ControlSignalOutputCallback signal_output_cb_;
};

}  // namespace shizuru::io
