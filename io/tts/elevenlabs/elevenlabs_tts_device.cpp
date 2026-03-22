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

ElevenLabsTtsDevice::~ElevenLabsTtsDevice() {
  if (worker_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      worker_stop_.store(true);
    }
    worker_cv_.notify_one();
    worker_thread_.join();
  }
}

std::string ElevenLabsTtsDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> ElevenLabsTtsDevice::GetPortDescriptors() const {
  return {
      {kTextIn,   PortDirection::kInput,  "text/plain"},
      {kAudioOut, PortDirection::kOutput, "audio/pcm"},
  };
}

// Non-blocking: post text to the worker thread instead of joining inline.
void ElevenLabsTtsDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (!active_.load()) { return; }
  if (port_name != kTextIn) {
    LOG_WARN("ElevenLabsTtsDevice: unsupported input port: {}", port_name);
    return;
  }
  const std::string text(frame.payload.begin(), frame.payload.end());
  if (text.empty()) { return; }

  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    text_queue_.push(text);
  }
  worker_cv_.notify_one();
}

void ElevenLabsTtsDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
}

void ElevenLabsTtsDevice::Start() {
  active_.store(true);
  worker_stop_.store(false);
  worker_thread_ = std::thread(&ElevenLabsTtsDevice::WorkerLoop, this);
}

void ElevenLabsTtsDevice::Stop() {
  active_.store(false);
  client_->Cancel();
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_stop_.store(true);
  }
  worker_cv_.notify_one();
  if (worker_thread_.joinable()) { worker_thread_.join(); }
}

void ElevenLabsTtsDevice::CancelSynthesis() {
  active_.store(false);
  client_->Cancel();
  // Drain the queue so no pending tasks run after cancel.
  std::lock_guard<std::mutex> lock(worker_mutex_);
  while (!text_queue_.empty()) { text_queue_.pop(); }
  active_.store(true);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void ElevenLabsTtsDevice::WorkerLoop() {
  while (true) {
    std::string text;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_cv_.wait(lock, [&] {
        return !text_queue_.empty() || worker_stop_.load();
      });
      if (worker_stop_.load() && text_queue_.empty()) { break; }
      text = std::move(text_queue_.front());
      text_queue_.pop();
    }
    Synthesize(text);
  }
}

void ElevenLabsTtsDevice::Synthesize(const std::string& text) {
  uint8_t carry     = 0;
  bool    has_carry = false;

  auto emit = [&](const uint8_t* buf, size_t byte_count) {
    if (byte_count == 0) { return; }
    DataFrame frame;
    frame.type          = "audio/pcm";
    frame.payload.assign(buf, buf + byte_count);
    frame.source_device = device_id_;
    frame.source_port   = kAudioOut;
    frame.timestamp     = std::chrono::steady_clock::now();
    OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
  };

  try {
    client_->Synthesize(text, [&](const void* data, size_t bytes) {
      if (!active_.load() || bytes == 0) { return; }

      const auto* src = static_cast<const uint8_t*>(data);
      size_t offset = 0;

      if (has_carry) {
        uint8_t pair[2] = {carry, src[0]};
        emit(pair, 2);
        has_carry = false;
        offset = 1;
      }

      const size_t remaining = bytes - offset;
      const size_t aligned   = (remaining / sizeof(int16_t)) * sizeof(int16_t);
      emit(src + offset, aligned);

      if (remaining % sizeof(int16_t) != 0) {
        carry     = src[offset + aligned];
        has_carry = true;
      }
    });
  } catch (const std::exception& e) {
    LOG_ERROR("ElevenLabsTtsDevice: synthesis error: {}", e.what());
  }
}

}  // namespace shizuru::io
