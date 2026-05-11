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
      {kItemIn,    PortDirection::kInput,  "",
                   runtime::PortPayloadKind::kConversationItem},
      {kAudioOut,  PortDirection::kOutput, "audio/pcm"},
      {kSignalIn,  PortDirection::kInput,  "",
                   runtime::PortPayloadKind::kControlSignal},
  };
}

void ElevenLabsTtsDevice::OnInput(const std::string& port_name, DataFrame frame) {
  // Legacy compatibility path remains only to satisfy the current IoDevice
  // interface contract. The voice pipeline no longer routes semantic output
  // through text_in, so this path should not be used by the exemplar.
  if (port_name != kTextIn) { return; }
  if (!active_.load()) { return; }

  const std::string text(frame.payload.begin(), frame.payload.end());
  if (text.empty()) { return; }

  LOG_WARN("ElevenLabsTtsDevice: legacy text_in path used; prefer item_in");
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    text_queue_.push(std::move(text));
  }
  worker_cv_.notify_one();
}

void ElevenLabsTtsDevice::OnConversationItem(const std::string& port_name,
                                             core::ConversationItem item) {
  if (port_name != kItemIn) { return; }
  if (!active_.load()) { return; }
  if (item.kind != core::ConversationItemKind::kAssistantMessage) { return; }

  std::string text;
  for (const auto& part : item.parts) {
    if (const auto* tp = std::get_if<core::TextPart>(&part)) {
      text += tp->text;
    }
  }
  if (text.empty()) { return; }

  LOG_INFO("ElevenLabsTtsDevice: queued synthesis from ConversationItem text_len={}",
           text.size());
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    text_queue_.push(std::move(text));
  }
  worker_cv_.notify_one();
}

void ElevenLabsTtsDevice::OnControlSignal(const std::string& port_name,
                                          core::ControlSignal signal) {
  (void)port_name;
  if (std::holds_alternative<core::CancelSignal>(signal) ||
      std::holds_alternative<core::InterruptSignal>(signal)) {
    LOG_INFO("ElevenLabsTtsDevice: typed signal_in received cancel-like signal");
    CancelSynthesis();
  }
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
  LOG_INFO("ElevenLabsTtsDevice: synthesizing text_len={}", text.size());
  // carry: holds a leftover byte when the HTTP chunk has odd length.
  // s16le PCM requires 2-byte alignment; we buffer the stray byte and
  // prepend it to the next chunk so every emitted payload is even-sized.
  uint8_t carry      = 0;
  bool    has_carry  = false;

  auto emit = [&](const uint8_t* buf, size_t byte_count) {
    if (byte_count == 0) { return; }

    // Assemble aligned data: [carry?] + buf[0..byte_count)
    const size_t total = (has_carry ? 1 : 0) + byte_count;
    const size_t emit_bytes = total & ~size_t{1};  // round down to even
    const size_t new_carry  = total - emit_bytes;   // 0 or 1

    if (emit_bytes == 0) {
      // Only 1 byte total — stash it and wait for more.
      carry     = has_carry ? carry : buf[0];
      has_carry = true;
      return;
    }

    DataFrame frame;
    frame.type          = "audio/pcm";
    frame.payload.reserve(emit_bytes);
    if (has_carry) { frame.payload.push_back(carry); }
    const size_t from_buf = emit_bytes - (has_carry ? 1 : 0);
    frame.payload.insert(frame.payload.end(), buf, buf + from_buf);

    // Update carry state.
    has_carry = (new_carry == 1);
    if (has_carry) { carry = buf[byte_count - 1]; }

    frame.source_device = device_id_;
    frame.source_port   = kAudioOut;
    frame.timestamp     = std::chrono::steady_clock::now();
    OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(output_cb_mutex_);
      cb = output_cb_;
    }
    if (cb) { cb(device_id_, kAudioOut, std::move(frame)); }
    LOG_DEBUG("ElevenLabsTtsDevice: emitted audio chunk bytes={}", emit_bytes);
  };

  try {
    client_->Synthesize(text, [&](const void* data, size_t bytes) {
      if (!active_.load() || bytes == 0) { return; }
      emit(static_cast<const uint8_t*>(data), bytes);
    });
  } catch (const std::exception& e) {
    LOG_ERROR("ElevenLabsTtsDevice: synthesis error: {}", e.what());
  }
}

}  // namespace shizuru::io
