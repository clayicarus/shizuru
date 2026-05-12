#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/content_part.h"
#include "core/conversation_item.h"
#include "core/control_signal.h"
#include "io/audio/audio_device/audio_frame.h"
#include "io/tts/tts_device.h"
#include "tts/baidu/baidu_tts_client.h"
#include "utils/baidu/baidu_config.h"
#include "utils/baidu/baidu_token_manager.h"

namespace shizuru::io {

// Baidu implementation of TtsDevice.
// Accepts assistant ConversationItems on "item_in", emits typed AudioFrames
// on "audio_out".
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
  void OnConversationItem(const std::string& port_name,
                          core::ConversationItem item) override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // TtsDevice interface
  void CancelSynthesis() override;

  // Block until the current synthesis + dispatch completes (or timeout).
  void WaitDone(std::chrono::milliseconds timeout);

 private:
  void Synthesize(const std::string& text);

  static constexpr char kItemIn[]   = "item_in";
  static constexpr char kSignalIn[] = "signal_in";
  static constexpr char kAudioOut[] = "audio_out";

  std::string device_id_;
  std::shared_ptr<services::BaiduTokenManager> token_mgr_;
  std::unique_ptr<services::BaiduTtsClient> client_;
  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  AudioFrameOutputCallback audio_output_cb_;

  std::mutex synth_mutex_;
  std::thread synth_thread_;

  std::mutex done_mutex_;
  std::condition_variable done_cv_;
};

}  // namespace shizuru::io
