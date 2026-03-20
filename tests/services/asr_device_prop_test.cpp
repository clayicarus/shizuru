// Property-based test for ASR device interface (Property 10)
// Feature: runtime-io-redesign, Property 10: ASR Device Audio-to-Text Transformation
// Uses RapidCheck + Google Test
//
// Tests the AsrDevice interface contract using a MockAsrDevice that
// transcribes audio DataFrames into text DataFrames.
// Validates: Requirements 7.2

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "io/asr/asr_device.h"
#include "io/data_frame.h"

namespace shizuru::io {
namespace {

// ---------------------------------------------------------------------------
// MockAsrDevice: implements AsrDevice, transcribes audio payload bytes
// to a fixed canned transcript string.
// ---------------------------------------------------------------------------
class MockAsrDevice : public AsrDevice {
 public:
  explicit MockAsrDevice(std::string transcript = "hello")
      : transcript_(std::move(transcript)) {}

  std::string GetDeviceId() const override { return "mock_asr"; }

  std::vector<PortDescriptor> GetPortDescriptors() const override {
    return {
        {"audio_in", PortDirection::kInput,  "audio/pcm"},
        {"text_out", PortDirection::kOutput, "text/plain"},
    };
  }

  void OnInput(const std::string& port_name, DataFrame frame) override {
    if (!active_.load()) { return; }
    if (port_name != "audio_in") { return; }
    if (frame.payload.empty()) { return; }

    // Emit a text/plain DataFrame on text_out.
    DataFrame out;
    out.type = "text/plain";
    out.payload = std::vector<uint8_t>(transcript_.begin(), transcript_.end());
    out.source_device = "mock_asr";
    out.source_port = "text_out";
    out.timestamp = std::chrono::steady_clock::now();

    OutputCallback cb;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cb = output_cb_;
    }
    if (cb) { cb("mock_asr", "text_out", std::move(out)); }
  }

  void SetOutputCallback(OutputCallback cb) override {
    std::lock_guard<std::mutex> lock(mu_);
    output_cb_ = std::move(cb);
  }

  void Start() override { active_.store(true); }
  void Stop()  override { active_.store(false); }

  // AsrDevice interface
  void CancelTranscription() override { Stop(); }

 private:
  std::string transcript_;
  std::atomic<bool> active_{false};
  mutable std::mutex mu_;
  OutputCallback output_cb_;
};

// ---------------------------------------------------------------------------
// Helper: build an audio/pcm DataFrame with random bytes as payload.
// ---------------------------------------------------------------------------
DataFrame AudioFrame(const std::vector<uint8_t>& bytes) {
  DataFrame f;
  f.type = "audio/pcm";
  f.payload = bytes;
  f.source_device = "mic";
  f.source_port = "audio_out";
  f.timestamp = std::chrono::steady_clock::now();
  return f;
}

// ---------------------------------------------------------------------------
// Property 10: ASR Device Audio-to-Text Transformation
// Feature: runtime-io-redesign, Property 10
// Validates: Requirements 7.2
// ---------------------------------------------------------------------------
RC_GTEST_PROP(AsrDevicePropTest, prop_audio_to_text_transformation, ()) {
  // Generate a non-empty audio payload (at least 1 byte).
  const auto audio_bytes = *rc::gen::nonEmpty(
      rc::gen::container<std::vector<uint8_t>>(rc::gen::arbitrary<uint8_t>()));

  MockAsrDevice device("transcribed_text");

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                                DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  device.OnInput("audio_in", AudioFrame(audio_bytes));

  // MockAsrDevice emits synchronously, so no wait needed.
  device.Stop();

  std::lock_guard<std::mutex> lock(mu);
  RC_ASSERT(!emitted.empty());

  // All emitted frames must be text/plain on text_out.
  for (const auto& f : emitted) {
    RC_ASSERT(f.type == "text/plain");
    RC_ASSERT(!f.payload.empty());
  }
}

// ---------------------------------------------------------------------------
// Unit test: stopped device discards audio frames
// ---------------------------------------------------------------------------
TEST(AsrDeviceTest, StoppedDeviceDiscardsAudioFrames) {
  MockAsrDevice device("should_not_appear");

  std::atomic<int> count{0};
  device.SetOutputCallback([&](const std::string&, const std::string&,
                                DataFrame) { ++count; });

  // Do NOT call Start().
  device.OnInput("audio_in", AudioFrame({0x01, 0x02}));

  EXPECT_EQ(count.load(), 0);
}

// ---------------------------------------------------------------------------
// Unit test: CancelTranscription stops the device
// ---------------------------------------------------------------------------
TEST(AsrDeviceTest, CancelTranscriptionStopsDevice) {
  MockAsrDevice device;
  device.Start();

  std::atomic<int> count{0};
  device.SetOutputCallback([&](const std::string&, const std::string&,
                                DataFrame) { ++count; });

  device.CancelTranscription();

  // After cancel, frames must be discarded.
  device.OnInput("audio_in", AudioFrame({0xAA, 0xBB}));
  EXPECT_EQ(count.load(), 0);
}

}  // namespace
}  // namespace shizuru::io
