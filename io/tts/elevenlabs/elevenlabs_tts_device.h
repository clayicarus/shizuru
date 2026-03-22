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

#include "io/tts/tts_device.h"
#include "tts/elevenlabs/elevenlabs_client.h"
#include "tts/tts_client.h"
#include "tts/config.h"

namespace shizuru::io {

// ElevenLabs implementation of TtsDevice.
// Wraps ElevenLabsClient: accepts text/plain DataFrames on "text_in",
// emits audio/pcm DataFrames on "audio_out".
// OnInput() is non-blocking: synthesis is posted to an internal worker thread.
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
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  // TtsDevice interface
  void CancelSynthesis() override;

 private:
  void WorkerLoop();
  void Synthesize(const std::string& text);

  static constexpr char kTextIn[]   = "text_in";
  static constexpr char kAudioOut[] = "audio_out";

  std::string device_id_;
  std::unique_ptr<services::TtsClient> client_;
  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  OutputCallback output_cb_;

  // Internal worker thread + task queue (replaces per-OnInput thread).
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  std::queue<std::string> text_queue_;
  std::thread worker_thread_;
  std::atomic<bool> worker_stop_{false};
};

}  // namespace shizuru::io
