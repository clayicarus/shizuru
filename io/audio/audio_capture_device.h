#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "io/io_device.h"
#include "audio_device/audio_recorder.h"

namespace shizuru::io {

// IoDevice wrapper around AudioRecorder.
// Emits typed AudioFrames on "audio_out" for each captured recorder frame.
class AudioCaptureDevice : public IoDevice {
 public:
  AudioCaptureDevice(std::unique_ptr<AudioRecorder> recorder,
                     std::string device_id = "audio_capture");

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void SetAudioFrameOutputCallback(AudioFrameOutputCallback cb) override;
  void Start() override;
  void Stop() override;

 private:
  static constexpr char kAudioOut[] = "audio_out";

  std::string device_id_;
  std::unique_ptr<AudioRecorder> recorder_;
  std::atomic<bool> active_{false};

  mutable std::mutex output_cb_mutex_;
  AudioFrameOutputCallback audio_output_cb_;
};

}  // namespace shizuru::io
