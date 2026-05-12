#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/control_signal.h"
#include "io/io_device.h"
#include "audio_device/audio_player.h"

namespace shizuru::io {

// IoDevice wrapper around AudioPlayer.
// Accepts typed AudioFrames on "audio_in" and writes them to the player.
class AudioPlayoutDevice : public IoDevice {
 public:
  AudioPlayoutDevice(std::unique_ptr<AudioPlayer> player,
                     std::string device_id = "audio_playout");

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnAudioFrame(const std::string& port_name, AudioFrame frame) override;
  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override;
  void Start() override;
  void Stop() override;

 private:
  static constexpr char kAudioIn[]   = "audio_in";
  static constexpr char kSignalIn[]  = "signal_in";

  std::string device_id_;
  std::unique_ptr<AudioPlayer> player_;
};

}  // namespace shizuru::io
