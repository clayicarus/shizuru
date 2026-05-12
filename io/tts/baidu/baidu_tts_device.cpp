#include "baidu_tts_device.h"

#include <chrono>
#include <utility>

#include "async_logger.h"

namespace shizuru::io {

BaiduTtsDevice::BaiduTtsDevice(services::BaiduConfig config,
                               std::string device_id)
    : device_id_(std::move(device_id)),
      token_mgr_(std::make_shared<services::BaiduTokenManager>(config)),
      client_(std::make_unique<services::BaiduTtsClient>(config, token_mgr_)) {}

BaiduTtsDevice::BaiduTtsDevice(services::BaiduConfig config,
                               std::shared_ptr<services::BaiduTokenManager> token_mgr,
                               std::string device_id)
    : device_id_(std::move(device_id)),
      token_mgr_(std::move(token_mgr)),
      client_(std::make_unique<services::BaiduTtsClient>(config, token_mgr_)) {}

std::string BaiduTtsDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> BaiduTtsDevice::GetPortDescriptors() const {
  return {
      {kItemIn,   PortDirection::kInput,  "",
                  runtime::PortPayloadKind::kConversationItem},
      {kSignalIn, PortDirection::kInput,  "",
                  runtime::PortPayloadKind::kControlSignal},
      {kAudioOut, PortDirection::kOutput, "audio/pcm",
                  runtime::PortPayloadKind::kAudioFrame},
  };
}

void BaiduTtsDevice::OnConversationItem(const std::string& port_name,
                                        core::ConversationItem item) {
  if (!active_.load()) { return; }
  if (port_name != kItemIn) {
    LOG_WARN("BaiduTtsDevice: unsupported input port: {}", port_name);
    return;
  }

  std::string text;
  for (const auto& part : item.parts) {
    if (const auto* tp = std::get_if<core::TextPart>(&part)) {
      text += tp->text;
    }
  }
  if (text.empty()) { return; }

  std::lock_guard<std::mutex> lock(synth_mutex_);
  if (synth_thread_.joinable()) { synth_thread_.join(); }
  synth_thread_ = std::thread([this, text] { Synthesize(text); });
}

void BaiduTtsDevice::OnControlSignal(const std::string& port_name,
                                     core::ControlSignal signal) {
  if (port_name != kSignalIn) { return; }
  if (std::holds_alternative<core::CancelSignal>(signal) ||
      std::holds_alternative<core::InterruptSignal>(signal)) {
    CancelSynthesis();
  }
}

void BaiduTtsDevice::SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  audio_output_cb_ = std::move(cb);
}

void BaiduTtsDevice::Start() { active_.store(true); }

void BaiduTtsDevice::Stop() {
  active_.store(false);
  std::lock_guard<std::mutex> lock(synth_mutex_);
  if (synth_thread_.joinable()) { synth_thread_.join(); }
}

void BaiduTtsDevice::CancelSynthesis() { Stop(); }

void BaiduTtsDevice::WaitDone(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(done_mutex_);
  done_cv_.wait_for(lock, timeout);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void BaiduTtsDevice::Synthesize(const std::string& text) {
  std::string mime;
  std::string audio;
  try {
    audio = client_->Synthesize(text, mime);
  } catch (const std::exception& e) {
    LOG_ERROR("BaiduTtsDevice: synthesis error: {}", e.what());
    done_cv_.notify_all();
    return;
  }

  if (!audio.empty()) {
    AudioFrame frame;
    frame.sample_rate = 16000;
    frame.channel_count = 1;
    const auto byte_count = audio.size() - (audio.size() % sizeof(int16_t));
    frame.sample_count = byte_count / sizeof(int16_t);
    std::memcpy(frame.data, audio.data(), byte_count);

    AudioFrameOutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = audio_output_cb_;
    }
    if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
  }

  done_cv_.notify_all();
}

}  // namespace shizuru::io
