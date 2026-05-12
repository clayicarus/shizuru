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

#include "core/conversation_item.h"
#include "core/control_signal.h"
#include "io/tts/tts_device.h"
#include "tts/elevenlabs/elevenlabs_client.h"
#include "tts/tts_client.h"
#include "tts/config.h"

namespace shizuru::io {

// ElevenLabs implementation of TtsDevice.
// Wraps ElevenLabsClient: accepts assistant ConversationItems on "item_in",
// emits typed AudioFrames on "audio_out".
// OnConversationItem() is non-blocking: synthesis is posted to an internal
// worker thread.
class ElevenLabsTtsDevice : public TtsDevice {
 public:
  // Production constructor: creates ElevenLabsClient from config.
  explicit ElevenLabsTtsDevice(services::ElevenLabsConfig config,
                               std::string device_id = "elevenlabs_tts");

  // Test constructor: inject any TtsClient (e.g. a mock).
  ElevenLabsTtsDevice(std::unique_ptr<services::TtsClient> client,
                      std::string device_id);

  ~ElevenLabsTtsDevice();

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

 private:
  void WorkerLoop();
  void Synthesize(const std::string& text);

  static constexpr char kItemIn[]    = "item_in";
  static constexpr char kAudioOut[]  = "audio_out";
  static constexpr char kSignalIn[]  = "signal_in";

  std::string device_id_;
  std::unique_ptr<services::TtsClient> client_;
  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  AudioFrameOutputCallback audio_output_cb_;

  // Internal worker thread + task queue (replaces per-OnInput thread).
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::queue<std::string> text_queue_;
  std::thread worker_thread_;
  std::atomic<bool> worker_stop_{false};
};

}  // namespace shizuru::io
