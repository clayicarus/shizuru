#include <gtest/gtest.h>

#include <vector>

#include "core/control_signal.h"
#include "io/audio/audio_device/audio_frame.h"
#include "io/vad/energy_vad_device.h"

namespace shizuru::io {
namespace {

AudioFrame MakeAudioFrame(const std::vector<int16_t>& samples) {
  AudioFrame frame;
  frame.sample_rate = 16000;
  frame.channel_count = 1;
  frame.sample_count = samples.size();
  for (size_t i = 0; i < samples.size(); ++i) {
    frame.data[i] = samples[i];
  }
  return frame;
}

TEST(EnergyVadDeviceTyped, SpeechStartEmitsInterruptSignalAndAudio) {
  EnergyVadConfig config;
  config.energy_threshold = 10.0F;
  config.rms_window_frames = 1;
  config.speech_onset_frames = 1;
  config.silence_hangover_frames = 2;
  config.pre_roll_frames = 1;

  EnergyVadDevice device(config);

  std::vector<core::ControlSignal> signals;
  std::vector<AudioFrame> audio_frames;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal signal) {
        signals.push_back(std::move(signal));
      });
  device.SetAudioFrameOutputCallback(
      [&](const std::string&, const std::string&, AudioFrame frame) {
        audio_frames.push_back(std::move(frame));
      });

  device.Start();
  device.OnAudioFrame(EnergyVadDevice::kAudioIn, MakeAudioFrame({200, 200}));
  device.Stop();

  ASSERT_EQ(signals.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<core::InterruptSignal>(signals[0]));
  ASSERT_EQ(audio_frames.size(), 1u);
  EXPECT_EQ(audio_frames[0].sample_count, 2u);
  EXPECT_EQ(audio_frames[0].data[0], 200);
}

TEST(EnergyVadDeviceTyped, SpeechEndEmitsFlushSignalAfterHangover) {
  EnergyVadConfig config;
  config.energy_threshold = 10.0F;
  config.rms_window_frames = 1;
  config.speech_onset_frames = 1;
  config.silence_hangover_frames = 2;
  config.pre_roll_frames = 0;

  EnergyVadDevice device(config);

  std::vector<core::ControlSignal> signals;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal signal) {
        signals.push_back(std::move(signal));
      });

  device.Start();
  device.OnAudioFrame(EnergyVadDevice::kAudioIn, MakeAudioFrame({300, 300}));
  device.OnAudioFrame(EnergyVadDevice::kAudioIn, MakeAudioFrame({0, 0}));
  device.OnAudioFrame(EnergyVadDevice::kAudioIn, MakeAudioFrame({0, 0}));
  device.Stop();

  ASSERT_EQ(signals.size(), 2u);
  EXPECT_TRUE(std::holds_alternative<core::InterruptSignal>(signals[0]));
  EXPECT_TRUE(std::holds_alternative<core::FlushSignal>(signals[1]));
}

}  // namespace
}  // namespace shizuru::io
