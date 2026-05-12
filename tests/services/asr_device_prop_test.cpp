#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/content_part.h"
#include "core/control_signal.h"
#include "core/conversation_item.h"
#include "io/audio/audio_device/audio_frame.h"
#include "io/asr/asr_device.h"

namespace shizuru::io {
namespace {

class MockTypedAsrDevice : public AsrDevice {
 public:
  explicit MockTypedAsrDevice(std::string transcript = "hello")
      : transcript_(std::move(transcript)) {}

  std::string GetDeviceId() const override { return "mock_asr"; }

  std::vector<PortDescriptor> GetPortDescriptors() const override {
    return {
        {"audio_in", PortDirection::kInput, "audio/pcm",
         runtime::PortPayloadKind::kAudioFrame},
        {"signal_in", PortDirection::kInput, "",
         runtime::PortPayloadKind::kControlSignal},
        {"item_out", PortDirection::kOutput, "",
         runtime::PortPayloadKind::kConversationItem},
    };
  }

  void OnAudioFrame(const std::string& port_name, AudioFrame frame) override {
    if (!active_.load() || port_name != "audio_in" || frame.sample_count == 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    buffered_audio_.push_back(std::move(frame));
  }

  void OnControlSignal(const std::string& port_name,
                       core::ControlSignal signal) override {
    if (!active_.load() || port_name != "signal_in") { return; }
    if (std::holds_alternative<core::FlushSignal>(signal)) {
      ConversationItemOutputCallback cb;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (buffered_audio_.empty()) { return; }
        buffered_audio_.clear();
        cb = item_output_cb_;
      }
      if (!cb) { return; }

      core::ConversationItem item;
      item.item_id = "asr_item";
      item.conversation_id = "conv";
      item.kind = core::ConversationItemKind::kUserMessage;
      item.actor = {"user", "User", core::ActorKind::kHuman};
      item.parts.emplace_back(core::TextPart{transcript_});
      item.wall_time = std::chrono::system_clock::now();
      cb("mock_asr", "item_out", std::move(item));
      return;
    }

    if (std::holds_alternative<core::CancelSignal>(signal) ||
        std::holds_alternative<core::InterruptSignal>(signal)) {
      std::lock_guard<std::mutex> lock(mu_);
      buffered_audio_.clear();
    }
  }

  void SetConversationItemOutputCallback(
      ConversationItemOutputCallback cb) override {
    std::lock_guard<std::mutex> lock(mu_);
    item_output_cb_ = std::move(cb);
  }

  void Start() override { active_.store(true); }

  void Stop() override { active_.store(false); }

  void CancelTranscription() override {
    std::lock_guard<std::mutex> lock(mu_);
    buffered_audio_.clear();
  }

 private:
  std::string transcript_;
  std::atomic<bool> active_{false};
  mutable std::mutex mu_;
  std::vector<AudioFrame> buffered_audio_;
  ConversationItemOutputCallback item_output_cb_;
};

io::AudioFrame MakeAudioFrame(const std::vector<int16_t>& samples) {
  io::AudioFrame frame;
  frame.sample_rate = 16000;
  frame.channel_count = 1;
  frame.sample_count = std::min(samples.size(), static_cast<size_t>(kMaxSamplesPerFrame));
  for (size_t i = 0; i < frame.sample_count; ++i) {
    frame.data[i] = samples[i];
  }
  return frame;
}

RC_GTEST_PROP(AsrDevicePropTest, prop_audio_flush_emits_conversation_item, ()) {
  const auto samples = *rc::gen::nonEmpty(
      rc::gen::container<std::vector<int16_t>>(rc::gen::arbitrary<int16_t>()));

  MockTypedAsrDevice device("transcribed_text");

  std::mutex mu;
  std::vector<core::ConversationItem> emitted;
  device.SetConversationItemOutputCallback(
      [&](const std::string&, const std::string&, core::ConversationItem item) {
        std::lock_guard<std::mutex> lock(mu);
        emitted.push_back(std::move(item));
      });

  device.Start();
  device.OnAudioFrame("audio_in", MakeAudioFrame(samples));
  device.OnControlSignal("signal_in", core::FlushSignal{});
  device.Stop();

  std::lock_guard<std::mutex> lock(mu);
  RC_ASSERT(emitted.size() == 1);
  RC_ASSERT(emitted[0].kind == core::ConversationItemKind::kUserMessage);
  RC_ASSERT(!emitted[0].parts.empty());
  const auto* text = std::get_if<core::TextPart>(&emitted[0].parts[0]);
  RC_ASSERT(text != nullptr);
  RC_ASSERT(text->text == "transcribed_text");
}

TEST(AsrDeviceTest, StoppedDeviceDiscardsTypedAudioFrames) {
  MockTypedAsrDevice device("should_not_appear");

  std::atomic<int> count{0};
  device.SetConversationItemOutputCallback(
      [&](const std::string&, const std::string&, core::ConversationItem) {
        ++count;
      });

  device.OnAudioFrame("audio_in", MakeAudioFrame({1, 2, 3}));
  device.OnControlSignal("signal_in", core::FlushSignal{});

  EXPECT_EQ(count.load(), 0);
}

TEST(AsrDeviceTest, CancelSignalClearsBufferedAudio) {
  MockTypedAsrDevice device("should_not_flush");

  std::atomic<int> count{0};
  device.SetConversationItemOutputCallback(
      [&](const std::string&, const std::string&, core::ConversationItem) {
        ++count;
      });

  device.Start();
  device.OnAudioFrame("audio_in", MakeAudioFrame({11, 22, 33}));
  device.OnControlSignal("signal_in", core::CancelSignal{});
  device.OnControlSignal("signal_in", core::FlushSignal{});
  device.Stop();

  EXPECT_EQ(count.load(), 0);
}

TEST(AsrDeviceTest, InterruptSignalClearsBufferedAudio) {
  MockTypedAsrDevice device("should_not_flush");

  std::atomic<int> count{0};
  device.SetConversationItemOutputCallback(
      [&](const std::string&, const std::string&, core::ConversationItem) {
        ++count;
      });

  device.Start();
  device.OnAudioFrame("audio_in", MakeAudioFrame({44, 55}));
  device.OnControlSignal("signal_in", core::InterruptSignal{});
  device.OnControlSignal("signal_in", core::FlushSignal{});
  device.Stop();

  EXPECT_EQ(count.load(), 0);
}

}  // namespace
}  // namespace shizuru::io
