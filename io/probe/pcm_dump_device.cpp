#include "pcm_dump_device.h"

#include <utility>

namespace shizuru::io {

PcmDumpDevice::PcmDumpDevice(std::string name) : name_(std::move(name)) {}

PcmDumpDevice::~PcmDumpDevice() { Stop(); }

std::string PcmDumpDevice::GetDeviceId() const { return name_; }

std::vector<PortDescriptor> PcmDumpDevice::GetPortDescriptors() const {
  return {
      {kPassIn,  PortDirection::kInput,  "audio/pcm",
                 runtime::PortPayloadKind::kAudioFrame},
      {kPassOut, PortDirection::kOutput, "audio/pcm",
                 runtime::PortPayloadKind::kAudioFrame},
  };
}

void PcmDumpDevice::SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) {
  audio_output_cb_ = std::move(cb);
}

void PcmDumpDevice::Start() {
  const std::string path = name_ + ".pcm";
  file_.open(path, std::ios::binary | std::ios::trunc);
}

void PcmDumpDevice::Stop() {
  if (file_.is_open()) { file_.close(); }
}

void PcmDumpDevice::OnAudioFrame(const std::string& port_name, AudioFrame frame) {
  if (port_name != kPassIn) { return; }

  if (file_.is_open() && frame.NumSamples() > 0) {
    file_.write(reinterpret_cast<const char*>(frame.data),
                static_cast<std::streamsize>(frame.NumSamples() * sizeof(int16_t)));
  }

  if (audio_output_cb_) {
    audio_output_cb_(name_, kPassOut, std::move(frame));
  }
}

}  // namespace shizuru::io
