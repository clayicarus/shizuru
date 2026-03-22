#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "io/asr/asr_device.h"
#include "asr/baidu/baidu_asr_client.h"
#include "utils/baidu/baidu_config.h"
#include "utils/baidu/baidu_token_manager.h"

namespace shizuru::io {

// Baidu implementation of AsrDevice.
// Accepts audio/pcm DataFrames on "audio_in", emits text/plain on "text_out".
// Accumulates audio until Flush() is called (or Stop()), then transcribes.
// Flush() is non-blocking: it posts a task to an internal worker thread.
class BaiduAsrDevice : public AsrDevice {
 public:
  // Creates its own BaiduTokenManager internally.
  BaiduAsrDevice(services::BaiduConfig config,
                 std::string device_id = "baidu_asr");

  // Shares an existing BaiduTokenManager (e.g. with BaiduTtsDevice).
  BaiduAsrDevice(services::BaiduConfig config,
                 std::shared_ptr<services::BaiduTokenManager> token_mgr,
                 std::string device_id = "baidu_asr");

  ~BaiduAsrDevice();

  // IoDevice interface
  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // AsrDevice interface
  void CancelTranscription() override;

  // Non-blocking: posts a transcription task to the internal worker thread.
  void Flush();

 private:
  void WorkerLoop();
  void Transcribe(std::vector<uint8_t> audio);

  static constexpr char kAudioIn[] = "audio_in";
  static constexpr char kTextOut[] = "text_out";

  std::string device_id_;
  services::BaiduConfig config_;
  std::shared_ptr<services::BaiduTokenManager> token_mgr_;
  std::unique_ptr<services::BaiduAsrClient> client_;

  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  OutputCallback output_cb_;

  std::mutex audio_mutex_;
  std::vector<uint8_t> audio_buffer_;

  // Internal worker thread + task queue (replaces per-Flush thread).
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::queue<std::function<void()>> task_queue_;
  std::thread worker_thread_;
  std::atomic<bool> worker_stop_{false};
};

}  // namespace shizuru::io
