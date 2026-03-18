#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tts/config.h"

namespace shizuru::services {

// Callback invoked for each audio chunk received from the TTS stream.
// data  — pointer to raw audio bytes (PCM s16le or compressed, per config)
// bytes — number of valid bytes in this chunk
using TtsAudioCallback = std::function<void(const void* data, size_t bytes)>;

// Abstract interface for a streaming TTS client.
class TtsClient {
 public:
  virtual ~TtsClient() = default;

  // Synthesize using a fully specified request. Streams audio chunks to
  // on_audio. Blocks until the stream is complete or cancelled.
  // Throws std::runtime_error on network or API errors.
  virtual void Synthesize(const TtsRequest& request,
                          TtsAudioCallback on_audio) = 0;

  // Convenience overload: synthesize plain text with default voice/settings.
  virtual void Synthesize(const std::string& text,
                          TtsAudioCallback on_audio) = 0;

  // Request cancellation of an in-progress synthesis.
  virtual void Cancel() = 0;
};

}  // namespace shizuru::services
