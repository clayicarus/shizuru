// Property-based and unit tests for ElevenLabsTtsDevice
// Feature: runtime-io-redesign
// Uses RapidCheck + Google Test

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "io/data_frame.h"
#include "tts/tts_client.h"
#include "tts/config.h"
#include "services/tts/elevenlabs/elevenlabs_tts_device.h"

namespace shizuru::services {
namespace {

// ---------------------------------------------------------------------------
// MockTtsClient: returns canned PCM audio bytes synchronously.
// ---------------------------------------------------------------------------
class MockTtsClient : public TtsClient {
 public:
  // Canned audio payload returned for every Synthesize call.
  std::vector<uint8_t> canned_audio{0x01, 0x02, 0x03, 0x04};
  std::atomic<int> synthesize_count{0};
  std::atomic<int> cancel_count{0};

  void Synthesize(const TtsRequest& /*request*/,
                  TtsAudioCallback on_audio) override {
    ++synthesize_count;
    if (on_audio && !canned_audio.empty()) {
      on_audio(canned_audio.data(), canned_audio.size());
    }
  }

  void Synthesize(const std::string& /*text*/,
                  TtsAudioCallback on_audio) override {
    ++synthesize_count;
    if (on_audio && !canned_audio.empty()) {
      on_audio(canned_audio.data(), canned_audio.size());
    }
  }

  void Cancel() override { ++cancel_count; }
};

// ---------------------------------------------------------------------------
// Helper: build a text/plain DataFrame from a string.
// ---------------------------------------------------------------------------
io::DataFrame TextFrame(const std::string& text) {
  io::DataFrame f;
  f.type = "text/plain";
  f.payload = std::vector<uint8_t>(text.begin(), text.end());
  f.source_device = "test";
  f.source_port = "out";
  f.timestamp = std::chrono::steady_clock::now();
  return f;
}

// Wait up to timeout_ms for predicate to become true.
bool WaitFor(std::function<bool()> pred, int timeout_ms = 500) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// ---------------------------------------------------------------------------
// Property 9: TTS Device Text-to-Audio Transformation
// Feature: runtime-io-redesign, Property 9
// Validates: Requirements 6.2
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ElevenLabsTtsDevicePropTest,
              prop_text_to_audio_transformation, ()) {
  // Generate a non-empty text string.
  const std::string text = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));

  auto mock = std::make_unique<MockTtsClient>();
  ElevenLabsTtsDevice device(std::move(mock), "tts_test");

  std::mutex mu;
  std::vector<io::DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                                io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  device.OnInput("text_in", TextFrame(text));

  bool got_audio = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  RC_ASSERT(got_audio);

  std::lock_guard<std::mutex> lock(mu);
  RC_ASSERT(!emitted.empty());
  // All emitted frames must be audio/pcm on audio_out.
  for (const auto& f : emitted) {
    RC_ASSERT(f.type == "audio/pcm");
    RC_ASSERT(!f.payload.empty());
  }
}

// ---------------------------------------------------------------------------
// Unit test: synthesis with mock client emits audio DataFrames
// ---------------------------------------------------------------------------
TEST(ElevenLabsTtsDeviceTest, SynthesisEmitsAudioDataFrames) {
  auto mock = std::make_unique<MockTtsClient>();
  mock->canned_audio = {0xAA, 0xBB, 0xCC};

  ElevenLabsTtsDevice device(std::move(mock), "tts_unit");

  std::mutex mu;
  std::vector<io::DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string& port,
                                io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
    (void)port;
  });

  device.Start();
  device.OnInput("text_in", TextFrame("hello"));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_FALSE(emitted.empty());
  EXPECT_EQ(emitted[0].type, "audio/pcm");
  EXPECT_EQ(emitted[0].payload, (std::vector<uint8_t>{0xAA, 0xBB, 0xCC}));
}

// ---------------------------------------------------------------------------
// Unit test: CancelSynthesis stops in-progress synthesis
// ---------------------------------------------------------------------------
TEST(ElevenLabsTtsDeviceTest, CancelSynthesisStopsDevice) {
  auto mock = std::make_unique<MockTtsClient>();
  MockTtsClient* mock_ptr = mock.get();

  ElevenLabsTtsDevice device(std::move(mock), "tts_cancel");
  device.SetOutputCallback([](const std::string&, const std::string&,
                               io::DataFrame) {});
  device.Start();
  device.CancelSynthesis();

  // CancelSynthesis delegates to Stop() which calls client_->Cancel().
  EXPECT_GE(mock_ptr->cancel_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Unit test: frame discarded when device is stopped
// ---------------------------------------------------------------------------
TEST(ElevenLabsTtsDeviceTest, FrameDiscardedWhenStopped) {
  auto mock = std::make_unique<MockTtsClient>();
  MockTtsClient* mock_ptr = mock.get();

  ElevenLabsTtsDevice device(std::move(mock), "tts_stopped");

  std::atomic<int> callback_count{0};
  device.SetOutputCallback([&](const std::string&, const std::string&,
                                io::DataFrame) { ++callback_count; });

  // Do NOT call Start() — device is stopped.
  device.OnInput("text_in", TextFrame("should be discarded"));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(mock_ptr->synthesize_count.load(), 0);
  EXPECT_EQ(callback_count.load(), 0);
}

}  // namespace
}  // namespace shizuru::services
