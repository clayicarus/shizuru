#include "pcm_dump_device.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace shizuru::io {

namespace {

AudioFrame AudioFrameFromDataFrame(const DataFrame& frame) {
  AudioFrame af;
  auto it = frame.metadata.find("sample_rate");
  if (it != frame.metadata.end()) {
    af.sample_rate = std::stoi(it->second);
  }
  it = frame.metadata.find("channel_count");
  if (it != frame.metadata.end()) {
    af.channel_count = static_cast<size_t>(std::stoi(it->second));
  }
  const size_t total_samples = frame.payload.size() / sizeof(int16_t);
  const size_t capped = std::min(total_samples, kMaxSamplesPerFrame);
  af.sample_count = af.channel_count == 0 ? 0 : capped / af.channel_count;
  std::memcpy(af.data, frame.payload.data(), capped * sizeof(int16_t));
  return af;
}

}  // namespace

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

void PcmDumpDevice::SetOutputCallback(OutputCallback cb) {
  output_cb_ = std::move(cb);
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

void PcmDumpDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (port_name != kPassIn) { return; }
  if (frame.type != "audio/pcm") { return; }
  OnAudioFrame(port_name, AudioFrameFromDataFrame(frame));
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
