#include "elevenlabs_tts_device.h"

#include <chrono>
#include <utility>

#include "async_logger.h"

namespace shizuru::io {

ElevenLabsTtsDevice::ElevenLabsTtsDevice(services::ElevenLabsConfig config,
                                         std::string device_id)
    : device_id_(std::move(device_id)),
      client_(std::make_unique<services::ElevenLabsClient>(std::move(config))) {}

ElevenLabsTtsDevice::ElevenLabsTtsDevice(std::unique_ptr<services::TtsClient> client,
                                         std::string device_id)
    : device_id_(std::move(device_id)), client_(std::move(client)) {}

std::string ElevenLabsTtsDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> ElevenLabsTtsDevice::GetPortDescriptors() const {
  return {
      {kTextIn,   PortDirection::kInput,  "text/plain"},
      {kAudioOut, PortDirection::kOutput, "audio/pcm"},
  };
}

void ElevenLabsTtsDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (!active_.load()) { return; }
  if (port_name != kTextIn) {
    LOG_WARN("ElevenLabsTtsDevice: unsupported input port: {}", port_name);
    return;
  }
  const std::string text(frame.payload.begin(), frame.payload.end());
  if (text.empty()) { return; }

  std::lock_guard<std::mutex> lock(synth_mutex_);
  if (synth_thread_.joinable()) { synth_thread_.join(); }
  synth_thread_ = std::thread([this, text] { Synthesize(text); });
}

void ElevenLabsTtsDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void ElevenLabsTtsDevice::Start() { active_.store(true); }

void ElevenLabsTtsDevice::Stop() {
  active_.store(false);
  client_->Cancel();
  std::lock_guard<std::mutex> lock(synth_mutex_);
  if (synth_thread_.joinable()) { synth_thread_.join(); }
}

void ElevenLabsTtsDevice::CancelSynthesis() { Stop(); }

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void ElevenLabsTtsDevice::Synthesize(const std::string& text) {
  try {
    client_->Synthesize(text, [this](const void* data, size_t bytes) {
      if (!active_.load() || bytes == 0) { return; }

      DataFrame frame;
      frame.type = "audio/pcm";
      frame.payload.assign(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + bytes);
      frame.source_device = device_id_;
      frame.source_port   = kAudioOut;
      frame.timestamp     = std::chrono::steady_clock::now();

      OutputCallback cb;
      {
        std::lock_guard<std::mutex> lock(output_cb_mutex_);
        cb = output_cb_;
      }
      if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
    });
  } catch (const std::exception& e) {
    LOG_ERROR("ElevenLabsTtsDevice: synthesis error: {}", e.what());
  }
}

}  // namespace shizuru::io
