#include "audio_playout_device.h"

#include <string>
#include <utility>

#include "async_logger.h"

namespace shizuru::io {

AudioPlayoutDevice::AudioPlayoutDevice(std::unique_ptr<AudioPlayer> player,
                                       std::string device_id)
    : device_id_(std::move(device_id)), player_(std::move(player)) {}

std::string AudioPlayoutDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> AudioPlayoutDevice::GetPortDescriptors() const {
  return {
      {kAudioIn,   PortDirection::kInput, "audio/pcm",
                   runtime::PortPayloadKind::kAudioFrame},
      {kSignalIn,  PortDirection::kInput, "",
                   runtime::PortPayloadKind::kControlSignal},
  };
}

void AudioPlayoutDevice::OnAudioFrame(const std::string& port_name,
                                      AudioFrame frame) {
  if (port_name != kAudioIn) { return; }
  if (!player_->IsPlaying()) { return; }
  if (frame.sample_count == 0) { return; }

  LOG_DEBUG("AudioPlayoutDevice: typed audio_in samples={} sample_rate={} channels={}",
            frame.sample_count, frame.sample_rate, frame.channel_count);
  player_->Write(frame);
}

void AudioPlayoutDevice::OnControlSignal(const std::string& port_name,
                                         core::ControlSignal signal) {
  (void)port_name;
  if (std::holds_alternative<core::CancelSignal>(signal) ||
      std::holds_alternative<core::InterruptSignal>(signal)) {
    LOG_INFO("AudioPlayoutDevice: typed signal_in received cancel-like signal");
    player_->Flush();
  }
}

void AudioPlayoutDevice::Start() { player_->Start(); }
void AudioPlayoutDevice::Stop()  { player_->Stop(); }

}  // namespace shizuru::io
