#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/control_signal.h"
#include "io/vad/vad_device.h"

namespace shizuru::io {

// Configuration for the energy-based VAD filter.
struct EnergyVadConfig {
  // RMS energy threshold (0–32767 scale for s16le).
  float energy_threshold = 500.0F;

  // Sliding window size (frames) for RMS max-filter.
  // is_speech = (max RMS over last rms_window_frames) >= energy_threshold.
  // Default: 10 frames (~200ms at 20ms/frame).
  size_t rms_window_frames = 10;

  // Consecutive speech frames required to confirm speech onset.
  size_t speech_onset_frames = 3;

  // Consecutive silence frames required to confirm speech end.
  size_t silence_hangover_frames = 15;  // ~300ms at 20ms/frame

  // Number of frames to buffer before speech_start so that the onset frames
  // themselves are not lost. Should be >= speech_onset_frames.
  // Default: same as speech_onset_frames (no extra pre-roll).
  size_t pre_roll_frames = 3;
};

// Energy-based VAD filter IoDevice.
//
// Combines VAD detection and audio gating into a single device.
// Incoming audio frames are analysed for speech energy; only frames that
// belong to a confirmed speech segment are forwarded on audio_out.
//
// Pre-roll buffering: the last `pre_roll_frames` audio frames are kept in a
// ring buffer. When speech_start fires, the buffered frames are flushed first
// so that the onset frames are not lost due to the onset confirmation delay.
//
// Port contract:
//   Input  "audio_in"  — audio/pcm (s16le)
//   Output "audio_out" — audio/pcm (speech frames only, with pre-roll)
//   Output "vad_out"   — vad/event (event name payload, optional — connect
//                        for observability or signal adaptation)
class EnergyVadDevice : public VadDevice {
 public:
  explicit EnergyVadDevice(EnergyVadConfig config = {},
                           std::string device_id = "vad");

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void OnAudioFrame(const std::string& port_name, AudioFrame frame) override;
  void SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) override;
  void SetControlSignalOutputCallback(ControlSignalOutputCallback cb) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

  static constexpr char kAudioIn[]  = "audio_in";
  static constexpr char kAudioOut[] = "audio_out";
  static constexpr char kVadOut[]   = "vad_out";
  static constexpr char kInterruptSignalOut[] = "interrupt_signal_out";
  static constexpr char kControlSignalOut[] = "control_signal_out";

 private:
  static float ComputeRms(const AudioFrame& frame);

  void EmitAudio(AudioFrame frame);
  void EmitEvent(const std::string& event);
  void EmitInterruptSignal();
  void EmitFlushSignal();
  void FlushPreRollTyped();

  std::string device_id_;
  EnergyVadConfig config_;
  std::atomic<bool> active_{false};

  // VAD state machine
  bool in_speech_{false};
  size_t onset_counter_{0};
  size_t hangover_counter_{0};

  // Sliding window of per-frame RMS values (max-filter).
  std::deque<float> rms_window_;

  // Pre-roll ring buffer: holds the last pre_roll_frames audio frames so
  // they can be replayed when speech_start fires.
  std::deque<AudioFrame> pre_roll_audio_buf_;

  mutable std::mutex output_cb_mutex_;
  OutputCallback output_cb_;
  AudioFrameOutputCallback audio_output_cb_;
  ControlSignalOutputCallback signal_output_cb_;
};

}  // namespace shizuru::io
