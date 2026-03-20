#pragma once

#include <atomic>
#include <memory>
#include <mutex>
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
class ElevenLabsTtsDevice : public TtsDevice {
 public:
  // Production constructor: creates ElevenLabsClient from config.
  explicit ElevenLabsTtsDevice(services::ElevenLabsConfig config,
                               std::string device_id = "elevenlabs_tts");

  // Test constructor: inject any TtsClient (e.g. a mock).
  ElevenLabsTtsDevice(std::unique_ptr<services::TtsClient> client,
                      std::string device_id);

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
  void Synthesize(const std::string& text);

  static constexpr char kTextIn[]   = "text_in";
  static constexpr char kAudioOut[] = "audio_out";

  std::string device_id_;
  std::unique_ptr<services::TtsClient> client_;
  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  OutputCallback output_cb_;

  std::mutex synth_mutex_;
  std::thread synth_thread_;
};

}  // namespace shizuru::io
