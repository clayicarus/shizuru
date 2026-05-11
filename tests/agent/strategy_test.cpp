// strategy_test.cpp — Stubbed after unified pipeline refactoring.
// TODO: Rewrite strategy tests to use new types.

#include <gtest/gtest.h>

#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::core {
namespace {

TEST(ResponseFilterTest, PassthroughReturnsInput) {
  PassthroughFilter filter;
  EXPECT_EQ(filter.Filter("hello"), "hello");
}

TEST(ResponseFilterTest, StripThinkingRemovesThinkTags) {
  StripThinkingFilter filter;
  EXPECT_EQ(filter.Filter("before<think>hidden</think>after"), "beforeafter");
}

TEST(TtsSegmentTest, PunctuationSegmentFlush) {
  PunctuationSegmentStrategy seg;
  seg.Append("Hello world.");
  std::string flushed = seg.Flush();
  EXPECT_EQ(flushed, "Hello world.");
}

}  // namespace
}  // namespace shizuru::core
