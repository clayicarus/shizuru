#pragma once

#include "io/io_device.h"

namespace shizuru::io {

// Vendor-agnostic VAD device interface.
// Accepts audio/pcm DataFrames on "audio_in".
// Emits vad/event DataFrames on "vad_out" (payload = event name bytes).
//
// Port contract:
//   Input  "audio_in" — DataFrames with type "audio/pcm" (s16le)
//   Output "vad_out"  — DataFrames with type "vad/event"
//
// vad/event payload:
//   "speech_start"  — voice activity detected, speech begins
//   "speech_end"    — silence detected after speech, speech ends
//   "speech_active" — speech is ongoing (periodic heartbeat)
class VadDevice : public IoDevice {};

}  // namespace shizuru::io
