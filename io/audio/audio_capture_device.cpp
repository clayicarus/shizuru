#include "audio_capture_device.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace shizuru::io {

AudioCaptureDevice::AudioCaptureDevice(std::unique_ptr<AudioRecorder> recorder,
                                       std::string device_id)
    : device_id_(std::move(device_id)), recorder_(std::move(recorder)) {}

std::string AudioCaptureDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> AudioCaptureDevice::GetPortDescriptors() const {
  return {{kAudioOut, PortDirection::kOutput, "audio/pcm"}};
}

void AudioCaptureDevice::OnInput(const std::string& /*port_name*/,
                                  DataFrame /*frame*/) {
  // Capture device has no inputs.
}

void AudioCaptureDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void AudioCaptureDevice::Start() {
  active_.store(true);
  recorder_->SetFrameCallback([this](const AudioFrame& af) {
    if (!active_.load()) { return; }

    // Convert AudioFrame → DataFrame (raw s16le bytes)
    const size_t byte_count = af.NumSamples() * sizeof(int16_t);
    DataFrame frame;
    frame.type = "audio/pcm";
    frame.payload.resize(byte_count);
    std::memcpy(frame.payload.data(), af.data, byte_count);
    frame.source_device = device_id_;
    frame.source_port   = kAudioOut;
    frame.timestamp     = std::chrono::steady_clock::now();
    frame.metadata["sample_rate"]   = std::to_string(af.sample_rate);
    frame.metadata["channel_count"] = std::to_string(af.channel_count);
    frame.metadata["sample_count"]  = std::to_string(af.sample_count);

    OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
  });
  recorder_->Start();
}

void AudioCaptureDevice::Stop() {
  active_.store(false);
  recorder_->Stop();
}

}  // namespace shizuru::io
