#include "baidu_asr_device.h"

#include <utility>

#include "async_logger.h"

namespace shizuru::io {

BaiduAsrDevice::BaiduAsrDevice(services::BaiduConfig config,
                               std::string device_id)
    : device_id_(std::move(device_id)),
      config_(config),
      token_mgr_(std::make_shared<services::BaiduTokenManager>(config)),
      client_(std::make_unique<services::BaiduAsrClient>(config, token_mgr_)) {}

BaiduAsrDevice::BaiduAsrDevice(services::BaiduConfig config,
                               std::shared_ptr<services::BaiduTokenManager> token_mgr,
                               std::string device_id)
    : device_id_(std::move(device_id)),
      config_(config),
      token_mgr_(std::move(token_mgr)),
      client_(std::make_unique<services::BaiduAsrClient>(config, token_mgr_)) {}

BaiduAsrDevice::~BaiduAsrDevice() {
  // Ensure worker thread is stopped cleanly.
  if (worker_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      worker_stop_.store(true);
    }
    worker_cv_.notify_one();
    worker_thread_.join();
  }
}

std::string BaiduAsrDevice::GetDeviceId() const { return device_id_; }

std::vector<PortDescriptor> BaiduAsrDevice::GetPortDescriptors() const {
  return {
      {kAudioIn,   PortDirection::kInput,  "audio/pcm",
                   runtime::PortPayloadKind::kAudioFrame},
      {kItemOut,   PortDirection::kOutput, "",
                   runtime::PortPayloadKind::kConversationItem},
      {kSignalIn,  PortDirection::kInput,  "",
                   runtime::PortPayloadKind::kControlSignal},
  };
}

void BaiduAsrDevice::OnAudioFrame(const std::string& port_name, AudioFrame frame) {
  if (!active_.load()) { return; }
  if (port_name != kAudioIn) { return; }
  if (frame.sample_count == 0) { return; }

  std::lock_guard<std::mutex> lock(audio_mutex_);
  const auto* begin = reinterpret_cast<const uint8_t*>(frame.data);
  const size_t byte_count = frame.NumSamples() * sizeof(int16_t);
  audio_buffer_.insert(audio_buffer_.end(), begin, begin + byte_count);
  LOG_DEBUG("BaiduAsrDevice: typed audio_in +{} bytes, buffer={} bytes",
            byte_count, audio_buffer_.size());
}

void BaiduAsrDevice::OnControlSignal(const std::string& port_name,
                                     core::ControlSignal signal) {
  (void)port_name;
  if (std::holds_alternative<core::FlushSignal>(signal)) {
    LOG_INFO("BaiduAsrDevice: typed signal_in received FlushSignal");
    Flush();
  } else if (std::holds_alternative<core::CancelSignal>(signal) ||
             std::holds_alternative<core::InterruptSignal>(signal)) {
    LOG_INFO("BaiduAsrDevice: typed signal_in received cancel-like signal");
    CancelTranscription();
  }
}

void BaiduAsrDevice::SetConversationItemOutputCallback(
    ConversationItemOutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  item_output_cb_ = std::move(cb);
}

void BaiduAsrDevice::SetOnItemCallback(AsrItemCallback cb) {
  std::lock_guard<std::mutex> lock(on_item_mutex_);
  on_item_ = std::move(cb);
}

void BaiduAsrDevice::Start() {
  active_.store(true);
  worker_stop_.store(false);
  worker_thread_ = std::thread(&BaiduAsrDevice::WorkerLoop, this);
}

void BaiduAsrDevice::Stop() {
  active_.store(false);
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_stop_.store(true);
  }
  worker_cv_.notify_one();
  if (worker_thread_.joinable()) { worker_thread_.join(); }
}

void BaiduAsrDevice::CancelTranscription() {
  std::lock_guard<std::mutex> lock(audio_mutex_);
  audio_buffer_.clear();
}

// Non-blocking: snapshot audio and post a task to the worker thread.
void BaiduAsrDevice::Flush() {
  if (!active_.load()) { return; }

  std::vector<uint8_t> audio;
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    audio.swap(audio_buffer_);
  }

  if (audio.empty()) {
    LOG_WARN("BaiduAsrDevice: Flush called with no audio data");
    return;
  }

  LOG_INFO("BaiduAsrDevice: flushing {} bytes to ASR", audio.size());

  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    task_queue_.push([this, audio = std::move(audio)]() mutable {
      Transcribe(std::move(audio));
    });
  }
  worker_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

void BaiduAsrDevice::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_cv_.wait(lock, [&] {
        return !task_queue_.empty() || worker_stop_.load();
      });
      if (worker_stop_.load() && task_queue_.empty()) { break; }
      task = std::move(task_queue_.front());
      task_queue_.pop();
    }
    task();
  }
}

void BaiduAsrDevice::Transcribe(std::vector<uint8_t> audio) {
  if (audio.empty()) { return; }

  const std::string audio_str(reinterpret_cast<const char*>(audio.data()),
                               audio.size());
  const std::string transcript = client_->Transcribe(audio_str, "audio/pcm");
  if (transcript.empty()) { return; }

  LOG_INFO("BaiduAsrDevice: ASR result: \"{}\"", transcript);

  // Deliver final result as ConversationItem to Core.
  // Only source-final results reach here — partial state never leaves this device.
  core::ConversationItem item;
  item.item_id = "asr:" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  item.conversation_id = "";  // Core assigns the active conversation.
  item.kind = core::ConversationItemKind::kUserMessage;
  item.actor = core::ActorRef{"local-user", "User", core::ActorKind::kHuman};
  item.parts.emplace_back(core::TextPart{transcript});
  item.wall_time = std::chrono::system_clock::now();

  AsrItemCallback item_cb;
  {
    std::lock_guard<std::mutex> lock(on_item_mutex_);
    item_cb = on_item_;
  }
  if (item_cb) {
    item_cb(item);
  }

  ConversationItemOutputCallback typed_cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    typed_cb = item_output_cb_;
  }
  if (typed_cb) {
    typed_cb(device_id_, kItemOut, std::move(item));
  }
}

}  // namespace shizuru::io
