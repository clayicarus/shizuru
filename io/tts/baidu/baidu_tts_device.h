#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "io/tts/tts_device.h"
#include "tts/baidu/baidu_tts_client.h"
#include "utils/baidu/baidu_config.h"
#include "utils/baidu/baidu_token_manager.h"

namespace shizuru::io {

// Baidu implementation of TtsDevice.
// Accepts text/plain DataFrames on "text_in", emits audio/pcm on "audio_out".
class BaiduTtsDevice : public TtsDevice {
 public:
  // Creates its own BaiduTokenManager internally.
  explicit BaiduTtsDevice(services::BaiduConfig config,
                          std::string device_id = "baidu_tts");

  // Shares an existing BaiduTokenManager (e.g. with BaiduAsrDevice).
  BaiduTtsDevice(services::BaiduConfig config,
                 std::shared_ptr<services::BaiduTokenManager> token_mgr,
                 std::string device_id = "baidu_tts");

  // IoDevice interface
  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // TtsDevice interface
  void CancelSynthesis() override;

  // Block until the current synthesis + dispatch completes (or timeout).
  void WaitDone(std::chrono::milliseconds timeout);

 private:
  void Synthesize(const std::string& text);

  static constexpr char kTextIn[]   = "text_in";
  static constexpr char kAudioOut[] = "audio_out";

  std::string device_id_;
  std::shared_ptr<services::BaiduTokenManager> token_mgr_;
  std::unique_ptr<services::BaiduTtsClient> client_;
  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  OutputCallback output_cb_;

  std::mutex synth_mutex_;
  std::thread synth_thread_;

  std::mutex done_mutex_;
  std::condition_variable done_cv_;
};

}  // namespace shizuru::io
