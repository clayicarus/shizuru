#pragma once

#include "io/io_device.h"

namespace shizuru::io {

// Vendor-agnostic VAD device interface.
// Accepts typed AudioFrames on "audio_in".
// Emits typed AudioFrames on "audio_out" and typed ControlSignals for
// speech lifecycle events.
//
// Port contract:
//   Input  "audio_in" — typed AudioFrames (s16le)
//   Output "audio_out" — speech-only AudioFrames
//   Output "interrupt_signal_out" — InterruptSignal on speech_start
//   Output "control_signal_out" — FlushSignal on speech_end
class VadDevice : public IoDevice {};

}  // namespace shizuru::io
