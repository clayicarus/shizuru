#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "tts/tts_client.h"
#include "tts/config.h"

namespace shizuru::services {

// ElevenLabs TTS client.
//
// Streaming endpoint: POST /v1/text-to-speech/{voice_id}/stream
//   Returns audio via chunked transfer encoding. Audio chunks are delivered
//   to TtsAudioCallback as they arrive — lowest latency path.
//
// Non-streaming endpoint: POST /v1/text-to-speech/{voice_id}
//   Returns the complete audio body in one response. Use SynthesizeFull().
class ElevenLabsClient : public TtsClient {
 public:
  explicit ElevenLabsClient(ElevenLabsConfig config);
  ~ElevenLabsClient() override;

  ElevenLabsClient(const ElevenLabsClient&) = delete;
  ElevenLabsClient& operator=(const ElevenLabsClient&) = delete;

  // TtsClient interface — uses the streaming endpoint.
  void Synthesize(const TtsRequest& request, TtsAudioCallback on_audio) override;
  void Synthesize(const std::string& text, TtsAudioCallback on_audio) override;

  // Non-streaming: waits for the full audio, then calls on_audio once.
  void SynthesizeFull(const TtsRequest& request, const TtsAudioCallback& on_audio);

  void Cancel() override;

 private:
  static constexpr char MODULE_NAME[] = "TTS";

  // Build query string for both endpoints.
  std::string QueryString() const;

  // Build the JSON request body from a TtsRequest.
  std::string BuildBody(const TtsRequest& request) const;

  // Resolve voice_id: use request's if set, else fall back to config default.
  const std::string& ResolveVoiceId(const TtsRequest& request) const;

  ElevenLabsConfig config_;
  std::atomic<bool> cancel_requested_{false};
  std::mutex request_mutex_;
};

}  // namespace shizuru::services
