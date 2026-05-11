#include "audio_capture_device.h"

#include <chrono>
#include <utility>

namespace shizuru::io {

AudioCaptureDevice::AudioCaptureDevice(std::unique_ptr<AudioRecorder> recorder,
                                       std::string device_id)
    : device_id_(std::move(device_id)), recorder_(std::move(recorder)) {}

std::string AudioCaptureDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> AudioCaptureDevice::GetPortDescriptors() const {
  return {{kAudioOut, PortDirection::kOutput, "audio/pcm",
           runtime::PortPayloadKind::kAudioFrame}};
}

void AudioCaptureDevice::OnInput(const std::string& /*port_name*/,
                                  DataFrame /*frame*/) {
  // Capture device has no inputs.
}

void AudioCaptureDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void AudioCaptureDevice::SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  audio_output_cb_ = std::move(cb);
}

void AudioCaptureDevice::Start() {
  active_.store(true);
  recorder_->SetFrameCallback([this](const AudioFrame& af) {
    if (!active_.load()) { return; }

    AudioFrameOutputCallback audio_cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      audio_cb = audio_output_cb_;
    }
    if (audio_cb) { audio_cb(device_id_, kAudioOut, af); }
  });
  recorder_->Start();
}

void AudioCaptureDevice::Stop() {
  active_.store(false);
  recorder_->Stop();
}

}  // namespace shizuru::io
