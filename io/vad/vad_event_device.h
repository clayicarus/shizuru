#pragma once

#include <string>
#include <vector>

#include "io/control_frame.h"
#include "io/interrupt_frame.h"
#include "io/io_device.h"

namespace shizuru::io {

// VadEventDevice — adapts VAD protocol events into runtime-level signals.
//
// Port contract:
//   Input  "vad_in"         — vad/event DataFrames from EnergyVadDevice
//   Output "vad_out"        — raw vad/event passthrough for observability
//   Output "interrupt_out"  — interrupt/request frames for barge-in handling
//   Output "control_out"    — control/command frames for device-side actions
//
// Mapping:
//   speech_start → interrupt_out (reason=barge_in, source=voice)
//   speech_end   → control_out   (command=flush)
//   other events → vad_out only
class VadEventDevice : public IoDevice {
 public:
  explicit VadEventDevice(std::string device_id = "vad_event");

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kVadIn[]  = "vad_in";
  static constexpr char kVadOut[] = "vad_out";
  static constexpr char kInterruptOut[] = "interrupt_out";
  static constexpr char kControlOut[] = "control_out";

 private:
  void EmitRawVadEvent(const DataFrame& frame);
  void EmitInterrupt();
  void EmitControl(std::string_view command);

  std::string device_id_;
  OutputCallback output_cb_;
};

}  // namespace shizuru::io
