#pragma once

#include "io/io_device.h"

namespace shizuru::io {

// Vendor-agnostic ASR device interface.
// Accepts typed AudioFrames on "audio_in" and emits final user
// ConversationItems on "item_out".
//
// Port contract:
//   Input  "audio_in" — typed AudioFrames
//   Output "item_out" — typed final ConversationItems
class AsrDevice : public IoDevice {
 public:
  // Cancels any in-progress transcription immediately.
  virtual void CancelTranscription() = 0;
};

}  // namespace shizuru::io
