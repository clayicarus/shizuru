#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "io/io_device.h"
#include "audio_device/audio_player.h"

namespace shizuru::io {

// IoDevice wrapper around AudioPlayer.
// Accepts audio/pcm DataFrames on "audio_in" and writes them to the player.
// Payload must be raw s16le PCM bytes.
class AudioPlayoutDevice : public IoDevice {
 public:
  AudioPlayoutDevice(std::unique_ptr<AudioPlayer> player,
                     std::string device_id = "audio_playout");

  std::string GetDeviceId() const override;
  std::vector<PortDescriptor> GetPortDescriptors() const override;
  void OnInput(const std::string& port_name, DataFrame frame) override;
  void SetOutputCallback(OutputCallback cb) override;
  void Start() override;
  void Stop() override;

 private:
  static constexpr char kAudioIn[] = "audio_in";

  std::string device_id_;
  std::unique_ptr<AudioPlayer> player_;
};

}  // namespace shizuru::io
