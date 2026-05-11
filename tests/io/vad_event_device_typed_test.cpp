#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "core/control_signal.h"
#include "io/vad/vad_event_device.h"

namespace shizuru::io {
namespace {

io::DataFrame MakeVadEventFrame(const std::string& event) {
  io::DataFrame frame;
  frame.type = "vad/event";
  frame.payload.assign(event.begin(), event.end());
  frame.timestamp = std::chrono::steady_clock::now();
  return frame;
}

TEST(VadEventDeviceTyped, SpeechStartEmitsTypedInterruptSignal) {
  VadEventDevice device;

  std::vector<core::ControlSignal> signals;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal signal) {
        signals.push_back(std::move(signal));
      });

  device.OnInput(VadEventDevice::kVadIn, MakeVadEventFrame("speech_start"));

  ASSERT_EQ(signals.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<core::InterruptSignal>(signals[0]));
}

TEST(VadEventDeviceTyped, SpeechEndEmitsTypedFlushSignal) {
  VadEventDevice device;

  std::vector<core::ControlSignal> signals;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal signal) {
        signals.push_back(std::move(signal));
      });

  device.OnInput(VadEventDevice::kVadIn, MakeVadEventFrame("speech_end"));

  ASSERT_EQ(signals.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<core::FlushSignal>(signals[0]));
}

}  // namespace
}  // namespace shizuru::io
