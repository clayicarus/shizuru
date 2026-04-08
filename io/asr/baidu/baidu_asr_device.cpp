#include "baidu_asr_device.h"

#include <chrono>
#include <utility>

#include "async_logger.h"
#include "io/control_frame.h"

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
      {kAudioIn,   PortDirection::kInput,  "audio/pcm"},
      {kTextOut,   PortDirection::kOutput, "text/plain"},
      {kControlIn, PortDirection::kInput,  "control/command"},
  };
}

void BaiduAsrDevice::OnInput(const std::string& port_name, DataFrame frame) {
  if (port_name == kControlIn) {
    const std::string cmd = ControlFrame::Parse(frame);
    LOG_INFO("BaiduAsrDevice: control_in received cmd '{}'", cmd);
    if (cmd == ControlFrame::kCommandFlush)  { Flush(); }
    else if (cmd == ControlFrame::kCommandCancel) { CancelTranscription(); }
    return;
  }
  if (!active_.load()) { return; }
  if (port_name != kAudioIn) {
    LOG_WARN("BaiduAsrDevice: unsupported input port: {}", port_name);
    return;
  }
  std::lock_guard<std::mutex> lock(audio_mutex_);
  audio_buffer_.insert(audio_buffer_.end(),
                       frame.payload.begin(), frame.payload.end());
  LOG_DEBUG("BaiduAsrDevice: audio_in +{} bytes, buffer={} bytes",
            frame.payload.size(), audio_buffer_.size());
}

void BaiduAsrDevice::SetOutputCallback(OutputCallback cb) {
  std::lock_guard<std::mutex> lock(output_cb_mutex_);
  output_cb_ = std::move(cb);
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

  DataFrame frame;
  frame.type          = "text/plain";
  frame.payload.assign(transcript.begin(), transcript.end());
  frame.source_device = device_id_;
  frame.source_port   = kTextOut;
  frame.timestamp     = std::chrono::steady_clock::now();

  OutputCallback cb;
  {
    std::lock_guard<std::mutex> lock(output_cb_mutex_);
    cb = output_cb_;
  }
  if (cb) { cb(device_id_, kTextOut, std::move(frame)); }
}

}  // namespace shizuru::io
