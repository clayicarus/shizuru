// tests/io/onebot_message_parse_test.cpp — OneBot message-chain parsing tests.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/content_part.h"
#include "io/onebot/onebot_device.h"

namespace shizuru::io::onebot {
namespace {

using json = nlohmann::json;

TEST(OneBotMessageParse, ArrayMessagePreservesSegmentOrderAcrossTextAndImage) {
  json message = json::array({
      {
          {"type", "text"},
          {"data", {{"text", "hello "}}},
      },
      {
          {"type", "image"},
          {"data", {{"url", "https://example.com/cat.png"}}},
      },
      {
          {"type", "text"},
          {"data", {{"text", " world"}}},
      },
  });

  auto parsed = OneBotDevice::ParseMessageForTest(message, "10001");

  ASSERT_EQ(parsed.parts.size(), 3);
  ASSERT_TRUE(std::holds_alternative<core::TextPart>(parsed.parts[0]));
  EXPECT_EQ(std::get<core::TextPart>(parsed.parts[0]).text, "hello ");
  ASSERT_TRUE(std::holds_alternative<core::ImagePart>(parsed.parts[1]));
  EXPECT_EQ(std::get<core::ImagePart>(parsed.parts[1]).url,
            "https://example.com/cat.png");
  ASSERT_TRUE(std::holds_alternative<core::TextPart>(parsed.parts[2]));
  EXPECT_EQ(std::get<core::TextPart>(parsed.parts[2]).text, " world");
}

TEST(OneBotMessageParse, ArrayMessageUsesStructuredPartsBeforeTagFallback) {
  json message = json::array({
      {
          {"type", "reply"},
          {"data", {{"id", "42"}}},
      },
      {
          {"type", "text"},
          {"data", {{"text", "ping "}}},
      },
      {
          {"type", "at"},
          {"data", {{"qq", "10001"}}},
      },
      {
          {"type", "face"},
          {"data", {{"id", "14"}}},
      },
      {
          {"type", "image"},
          {"data", {{"url", "https://example.com/ping.png"}}},
      },
  });

  auto parsed = OneBotDevice::ParseMessageForTest(message, "10001");

  ASSERT_TRUE(parsed.reply_to.has_value());
  EXPECT_EQ(*parsed.reply_to, "onebot:42");
  ASSERT_EQ(parsed.parts.size(), 2);
  ASSERT_TRUE(std::holds_alternative<core::TextPart>(parsed.parts[0]));
  EXPECT_EQ(std::get<core::TextPart>(parsed.parts[0]).text,
            "ping <at id=\"self\"/><face desc=\"微笑\"/>");
  ASSERT_TRUE(std::holds_alternative<core::ImagePart>(parsed.parts[1]));
  EXPECT_EQ(std::get<core::ImagePart>(parsed.parts[1]).url,
            "https://example.com/ping.png");
}

TEST(OneBotMessageParse, CqStringPreservesImageOrderingAndReplyReference) {
  const std::string message =
      "before[CQ:image,url=https://example.com/a.png]after[CQ:reply,id=99]";

  auto parsed = OneBotDevice::ParseMessageForTest(message, "10001");

  ASSERT_TRUE(parsed.reply_to.has_value());
  EXPECT_EQ(*parsed.reply_to, "onebot:99");
  ASSERT_EQ(parsed.parts.size(), 3);
  ASSERT_TRUE(std::holds_alternative<core::TextPart>(parsed.parts[0]));
  EXPECT_EQ(std::get<core::TextPart>(parsed.parts[0]).text, "before");
  ASSERT_TRUE(std::holds_alternative<core::ImagePart>(parsed.parts[1]));
  EXPECT_EQ(std::get<core::ImagePart>(parsed.parts[1]).url,
            "https://example.com/a.png");
  ASSERT_TRUE(std::holds_alternative<core::TextPart>(parsed.parts[2]));
  EXPECT_EQ(std::get<core::TextPart>(parsed.parts[2]).text, "after");
}

}  // namespace
}  // namespace shizuru::io::onebot
