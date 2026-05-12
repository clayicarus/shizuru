#pragma once

#include <string>

namespace shizuru::io {

// Vendor-agnostic ASR request type.
// The actual audio data flows as typed AudioFrames through the device ports;
// this struct is an informational type for configuration and metadata.
struct AsrRequest {
  std::string audio_format;   // e.g. "audio/pcm", "audio/wav"
  std::string language_hint;  // BCP-47 language hint, e.g. "zh-CN"
};

// Vendor-agnostic ASR result type.
// The actual transcript flows as ConversationItems on the item_out port;
// this struct is an informational type for result metadata.
struct AsrResult {
  std::string transcript;     // Recognized text
  bool success = false;
  std::string error_message;  // Non-empty on failure
};

}  // namespace shizuru::io
