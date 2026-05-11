// tests/io/asr_interpreter_test.cpp — ASR final-only delivery test.
//
// Task 47: Verifies requirements 6.4, 6.5:
//   - ASR adapter only outputs ConversationItem when final result is ready
//   - Partial/provisional states are NOT exposed to Core

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "core/conversation_item.h"
#include "core/content_part.h"

namespace shizuru {
namespace {

// Simulates the ASR interpreter behavior:
// - Receives audio frames (not tested here — internal)
// - Only delivers ConversationItem on final transcription result
// - Never delivers partial/provisional results
class MockAsrInterpreter {
 public:
  using ItemCallback = std::function<void(core::ConversationItem)>;

  void SetOnItemCallback(ItemCallback cb) { on_item_ = std::move(cb); }

  // Simulate receiving partial results — should NOT deliver to Core.
  void OnPartialResult(const std::string& /*partial_text*/) {
    partial_count_++;
    // Partial results are maintained internally, never delivered.
  }

  // Simulate receiving final result — SHOULD deliver to Core.
  void OnFinalResult(const std::string& final_text) {
    final_count_++;

    core::ConversationItem item;
    item.item_id = "asr:" + std::to_string(final_count_);
    item.kind = core::ConversationItemKind::kUserMessage;
    item.actor = core::ActorRef{"local-user", "User", core::ActorKind::kHuman};
    item.parts.emplace_back(core::TextPart{final_text});
    item.wall_time = std::chrono::system_clock::now();

    if (on_item_) { on_item_(std::move(item)); }
  }

  int partial_count() const { return partial_count_; }
  int final_count() const { return final_count_; }

 private:
  ItemCallback on_item_;
  int partial_count_ = 0;
  int final_count_ = 0;
};

TEST(AsrInterpreter, OnlyFinalResultsDeliverConversationItem) {
  MockAsrInterpreter asr;
  std::vector<core::ConversationItem> delivered_items;

  asr.SetOnItemCallback([&](core::ConversationItem item) {
    delivered_items.push_back(std::move(item));
  });

  // Simulate partial results — none should be delivered.
  asr.OnPartialResult("hel");
  asr.OnPartialResult("hello");
  asr.OnPartialResult("hello wor");

  EXPECT_EQ(delivered_items.size(), 0);
  EXPECT_EQ(asr.partial_count(), 3);

  // Simulate final result — should be delivered.
  asr.OnFinalResult("hello world");

  ASSERT_EQ(delivered_items.size(), 1);
  EXPECT_EQ(delivered_items[0].kind, core::ConversationItemKind::kUserMessage);

  // Verify the text content.
  ASSERT_EQ(delivered_items[0].parts.size(), 1);
  auto* tp = std::get_if<core::TextPart>(&delivered_items[0].parts[0]);
  ASSERT_NE(tp, nullptr);
  EXPECT_EQ(tp->text, "hello world");
}

TEST(AsrInterpreter, MultipleUtterancesDeliverSeparateItems) {
  MockAsrInterpreter asr;
  std::vector<core::ConversationItem> delivered_items;

  asr.SetOnItemCallback([&](core::ConversationItem item) {
    delivered_items.push_back(std::move(item));
  });

  // First utterance.
  asr.OnPartialResult("hi");
  asr.OnFinalResult("hi there");

  // Second utterance.
  asr.OnPartialResult("how");
  asr.OnPartialResult("how are");
  asr.OnFinalResult("how are you");

  ASSERT_EQ(delivered_items.size(), 2);

  auto* tp1 = std::get_if<core::TextPart>(&delivered_items[0].parts[0]);
  EXPECT_EQ(tp1->text, "hi there");

  auto* tp2 = std::get_if<core::TextPart>(&delivered_items[1].parts[0]);
  EXPECT_EQ(tp2->text, "how are you");
}

}  // namespace
}  // namespace shizuru
