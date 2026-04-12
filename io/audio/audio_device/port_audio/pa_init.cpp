#include "audio_device/port_audio/pa_init.h"
#include <portaudio.h>
#include <stdexcept>
#include <string>

namespace shizuru::io {

void EnsurePaInitialized() {
  static bool initialized = [] {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      throw std::runtime_error(
          std::string("PortAudio init failed: ") + Pa_GetErrorText(err));
    }
    // Do NOT register Pa_Terminate via atexit.  The atexit handler runs
    // during static destruction / process exit, at which point PaPlayer
    // and PaRecorder streams may still be open.  Pa_Terminate calls
    // CloseOpenStreams → AbortStream → AudioOutputUnitStop, which blocks
    // waiting for the CoreAudio IO thread callback to finish.  If that
    // callback is itself waiting on a lock held by the main thread (e.g.
    // DispatchFrame's shared_mutex), the process deadlocks and aborts.
    //
    // Instead, streams are closed explicitly by PaPlayer::Stop() and
    // PaRecorder::Stop() (called from device destructors and
    // AgentRuntime::Shutdown).  The OS reclaims all resources on exit.
    return true;
  }();
  (void)initialized;
}

}  // namespace shizuru::io
