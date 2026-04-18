// Unit + property-based tests for VadEventDevice
// Feature: core-decoupling, Property 6: VadEventDevice signal adaptation

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "io/control_frame.h"
#include "io/data_frame.h"
#include "io/interrupt_frame.h"
#include "io/vad/vad_event_device.h"

namespace shizuru::io {
namespace {

// Helper: build a vad/event DataFrame with the given event name as payload.
DataFrame MakeVadFrame(const std::string& event) {
  DataFrame f;
  f.type      = "vad/event";
  f.payload   = std::vector<uint8_t>(event.begin(), event.end());
  f.timestamp = std::chrono::steady_clock::now();
  return f;
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

TEST(VadEventDeviceTest, GetDeviceIdDefault) {
  VadEventDevice dev;
  EXPECT_EQ(dev.GetDeviceId(), "vad_event");
}

TEST(VadEventDeviceTest, GetDeviceIdCustom) {
  VadEventDevice dev("my_vad");
  EXPECT_EQ(dev.GetDeviceId(), "my_vad");
}

TEST(VadEventDeviceTest, PortDescriptorsContainAdaptedOutputs) {
  VadEventDevice dev;
  const auto ports = dev.GetPortDescriptors();
  bool has_in = false;
  bool has_vad_out = false;
  bool has_interrupt_out = false;
  bool has_control_out = false;
  for (const auto& p : ports) {
    if (p.name == VadEventDevice::kVadIn && p.direction == PortDirection::kInput) {
      has_in = true;
    }
    if (p.name == VadEventDevice::kVadOut &&
        p.direction == PortDirection::kOutput &&
        p.data_type == "vad/event") {
      has_vad_out = true;
    }
    if (p.name == VadEventDevice::kInterruptOut &&
        p.direction == PortDirection::kOutput &&
        p.data_type == InterruptFrame::kType) {
      has_interrupt_out = true;
    }
    if (p.name == VadEventDevice::kControlOut &&
        p.direction == PortDirection::kOutput &&
        p.data_type == "control/command") {
      has_control_out = true;
    }
  }
  EXPECT_TRUE(has_in);
  EXPECT_TRUE(has_vad_out);
  EXPECT_TRUE(has_interrupt_out);
  EXPECT_TRUE(has_control_out);
}

TEST(VadEventDeviceTest, SpeechEndEmitsFlushOnControlOut) {
  VadEventDevice dev;

  std::string emitted_port;
  std::string emitted_command;

  dev.SetOutputCallback([&](const std::string& /*device_id*/,
                            const std::string& port,
                            DataFrame frame) {
    if (port != VadEventDevice::kControlOut) { return; }
    emitted_port = port;
    emitted_command = ControlFrame::Parse(frame);
  });

  dev.OnInput(VadEventDevice::kVadIn, MakeVadFrame("speech_end"));

  EXPECT_EQ(emitted_port, VadEventDevice::kControlOut);
  EXPECT_EQ(emitted_command, ControlFrame::kCommandFlush);
}

TEST(VadEventDeviceTest, SpeechStartEmitsInterruptOnInterruptOut) {
  VadEventDevice dev;

  std::string emitted_reason;
  std::string emitted_source;
  dev.SetOutputCallback([&](const std::string&, const std::string&, DataFrame frame) {
    if (frame.type != InterruptFrame::kType) { return; }
    emitted_reason = InterruptFrame::ParseReason(frame);
    emitted_source = InterruptFrame::ParseSource(frame);
  });

  dev.OnInput(VadEventDevice::kVadIn, MakeVadFrame("speech_start"));
  EXPECT_EQ(emitted_reason, InterruptFrame::kReasonBargeIn);
  EXPECT_EQ(emitted_source, "voice");
}

TEST(VadEventDeviceTest, RawVadEventStillEmittedOnVadOut) {
  VadEventDevice dev;

  std::string emitted_event;
  dev.SetOutputCallback([&](const std::string&, const std::string& port, DataFrame frame) {
    if (port != VadEventDevice::kVadOut) { return; }
    emitted_event = std::string(frame.payload.begin(), frame.payload.end());
  });

  dev.OnInput(VadEventDevice::kVadIn, MakeVadFrame("speech_active"));
  EXPECT_EQ(emitted_event, "speech_active");
}

TEST(VadEventDeviceTest, NoCallbackDoesNotCrash) {
  VadEventDevice dev;
  // No SetOutputCallback — must not crash.
  EXPECT_NO_THROW(dev.OnInput(VadEventDevice::kVadIn, MakeVadFrame("speech_end")));
}

TEST(VadEventDeviceTest, WrongPortIsIgnored) {
  VadEventDevice dev;

  bool called = false;
  dev.SetOutputCallback([&](const std::string&, const std::string&, DataFrame) {
    called = true;
  });

  dev.OnInput("wrong_port", MakeVadFrame("speech_end"));
  EXPECT_FALSE(called);
}

TEST(VadEventDeviceTest, EmittedInterruptFrameTypeIsInterruptRequest) {
  VadEventDevice dev;

  std::string emitted_type;
  dev.SetOutputCallback([&](const std::string&, const std::string&, DataFrame frame) {
    if (frame.type == InterruptFrame::kType) {
      emitted_type = frame.type;
    }
  });

  dev.OnInput(VadEventDevice::kVadIn, MakeVadFrame("speech_start"));
  EXPECT_EQ(emitted_type, InterruptFrame::kType);
}

// ---------------------------------------------------------------------------
// Property 6: VadEventDevice raw passthrough emit
// Validates: Requirements 8.2
// ---------------------------------------------------------------------------

RC_GTEST_PROP(VadEventDevicePropTest, prop_raw_passthrough_emit, (void)) {
  // Feature: core-decoupling, Property 6: VadEventDevice raw passthrough emit
  //
  // For any non-empty event name, delivering a vad/event frame on vad_in must
  // cause vad_out to emit a frame with identical payload bytes.

  const auto event = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange<char>('a', 'z')));

  VadEventDevice dev;

  std::vector<uint8_t> captured_payload;
  bool callback_called = false;

  dev.SetOutputCallback([&](const std::string& /*device_id*/,
                            const std::string& port,
                            DataFrame frame) {
    RC_ASSERT(port == VadEventDevice::kVadOut);
    captured_payload = frame.payload;
    callback_called  = true;
  });

  dev.OnInput(VadEventDevice::kVadIn, MakeVadFrame(event));

  RC_ASSERT(callback_called);

  const std::string result(captured_payload.begin(), captured_payload.end());
  RC_ASSERT(result == event);
}

}  // namespace
}  // namespace shizuru::io
