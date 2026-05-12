#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace shizuru::io {

// Vendor-agnostic TTS request type.
// The actual text data flows as ConversationItems through the device ports;
// this struct is an informational type for configuration and metadata.
struct TtsRequest {
  std::string text;         // Text to synthesize
  std::string language;     // BCP-47 language tag, e.g. "en-US"
  std::string voice_id;     // Vendor-agnostic voice identifier
};

// Vendor-agnostic TTS result type.
// The actual audio data flows as typed AudioFrames on the audio_out port;
// this struct is an informational type for result metadata.
struct TtsResult {
  std::vector<uint8_t> audio_data;  // Synthesized PCM audio bytes
  bool success = false;
  std::string error_message;        // Non-empty on failure
};

}  // namespace shizuru::io
