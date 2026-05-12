#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "io/io_device.h"

namespace shizuru::io {

// A probe IoDevice that dumps raw AudioFrames to a file.
// No WAV header — raw s16le bytes are written directly.
//
// Port contract:
//   Input  "pass_in"  — accepts typed AudioFrames
//   Output "pass_out" — re-emits the same AudioFrame unchanged (chainable)
//
// The output file is opened on Start() and closed on Stop().
// File path: <name>.pcm (relative to working directory).
class PcmDumpDevice : public IoDevice {
 public:
  // name: used as device_id and as the stem of the output filename (<name>.pcm)
  explicit PcmDumpDevice(std::string name);
  ~PcmDumpDevice() override;

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnAudioFrame(const std::string& port_name, AudioFrame frame) override;
  void SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kPassIn[]  = "pass_in";
  static constexpr char kPassOut[] = "pass_out";

 private:
  std::string name_;
  std::ofstream file_;
  AudioFrameOutputCallback audio_output_cb_;
};

}  // namespace shizuru::io
