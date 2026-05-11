#pragma once

#include "io/io_device.h"

namespace shizuru::io {

// Vendor-agnostic TTS device interface.
// The semantic pipeline drives TTS with assistant ConversationItems on
// "item_in"; implementations emit typed AudioFrames on "audio_out".
class TtsDevice : public IoDevice {
 public:
  // Cancels any in-progress synthesis immediately.
  virtual void CancelSynthesis() = 0;
};

}  // namespace shizuru::io
