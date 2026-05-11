#pragma once

#include "io/io_device.h"

namespace shizuru::io {

// Vendor-agnostic ASR device interface.
// Accepts audio on "audio_in". Implementations may expose both:
// - legacy text/plain DataFrame output on "text_out" for transcript probes
// - typed ConversationItem output on "item_out" for semantic delivery to Core
//
// Port contract:
//   Input  "audio_in" — accepts DataFrames with type "audio/pcm"
//   Output "text_out" — optional legacy DataFrame transcript output
//   Output "item_out" — optional typed ConversationItem final result
class AsrDevice : public IoDevice {
 public:
  // Cancels any in-progress transcription immediately.
  virtual void CancelTranscription() = 0;
};

}  // namespace shizuru::io
