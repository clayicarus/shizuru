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
      {kTextIn,   PortDirection::kInput,  "text/plain"},
      {kAudioOut, PortDirection::kOutput, "audio/pcm"},
  };
}

void BaiduTtsDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (!active_.load()) { return; }
  if (port_name != kTextIn) {
    LOG_WARN("BaiduTtsDevice: unsupported input port: {}", port_name);
    return;
  }
  const std::string text(frame.payload.begin(), frame.payload.end());
  if (text.empty()) { return; }

  std::lock_guard<std::mutex> lock(synth_mutex_);
  if (synth_thread_.joinable()) { synth_thread_.join(); }
  synth_thread_ = std::thread([this, text] { Synthesize(text); });
}

void BaiduTtsDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
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
    DataFrame frame;
    frame.type = "audio/pcm";
    frame.payload.assign(
        reinterpret_cast<const uint8_t*>(audio.data()),
        reinterpret_cast<const uint8_t*>(audio.data()) + audio.size());
    frame.source_device = device_id_;
    frame.source_port   = kAudioOut;
    frame.timestamp     = std::chrono::steady_clock::now();
    frame.metadata["sample_rate"]   = "16000";
    frame.metadata["channel_count"] = "1";

    OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
  }

  done_cv_.notify_all();
}

}  // namespace shizuru::io
