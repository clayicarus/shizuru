#include "audio_playout_device.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <utility>

#include "io/control_frame.h"

namespace shizuru::io {

AudioPlayoutDevice::AudioPlayoutDevice(std::unique_ptr<AudioPlayer> player,
                                       std::string device_id)
    : device_id_(std::move(device_id)), player_(std::move(player)) {}

std::string AudioPlayoutDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> AudioPlayoutDevice::GetPortDescriptors() const {
  return {
      {kAudioIn,   PortDirection::kInput, "audio/pcm"},
      {kControlIn, PortDirection::kInput, "control/command"},
  };
}

void AudioPlayoutDevice::OnInput(const std::string& port_name,
                                  DataFrame frame) {
  if (port_name == kControlIn) {
    const std::string cmd = ControlFrame::Parse(frame);
    if (cmd == ControlFrame::kCommandCancel) { player_->Flush(); }
    return;
  }
  if (port_name != kAudioIn) { return; }
  if (frame.payload.empty()) { return; }
  if (!player_->IsPlaying()) { return; }  // Discard when playout is stopped.

  int    sample_rate   = 16000;
  size_t channel_count = 1;

  auto it = frame.metadata.find("sample_rate");
  if (it != frame.metadata.end()) { sample_rate = std::stoi(it->second); }
  it = frame.metadata.find("channel_count");
  if (it != frame.metadata.end()) {
    channel_count = static_cast<size_t>(std::stoi(it->second));
  }

  // AudioFrame::data is a fixed-size stack buffer (kMaxSamplesPerFrame).
  // Slice the payload into chunks that fit, and write each one.
  const auto* src       = reinterpret_cast<const int16_t*>(frame.payload.data());
  const size_t total    = frame.payload.size() / sizeof(int16_t);
  size_t       written  = 0;

  while (written < total) {
    const size_t chunk = std::min(total - written, kMaxSamplesPerFrame);

    AudioFrame af;
    af.sample_rate   = sample_rate;
    af.channel_count = channel_count;
    af.sample_count  = chunk;
    std::memcpy(af.data, src + written, chunk * sizeof(int16_t));
    player_->Write(af);

    written += chunk;
  }
}

void AudioPlayoutDevice::SetOutputCallback(OutputCallback /*cb*/) {
  // Playout device has no outputs.
}

void AudioPlayoutDevice::Start() { player_->Start(); }
void AudioPlayoutDevice::Stop()  { player_->Stop(); }

}  // namespace shizuru::io
