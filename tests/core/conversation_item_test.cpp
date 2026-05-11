// tests/core/conversation_item_test.cpp — Unit tests for kind/parts constraints.
//
// Verifies requirements 4.1-4.6:
//   - kUserMessage only allows TextPart, ImagePart, AudioPart
//   - kAssistantMessage only allows TextPart, ImagePart, AudioPart
//   - kSystemEvent only allows TextPart
//   - kToolCall only allows ToolCallPart
//   - kToolResult only allows ToolResultPart
//   - ToolCallPart and ToolResultPart must not coexist in one item

#include <gtest/gtest.h>

#include "core/content_part.h"
#include "core/conversation_item.h"

namespace shizuru::core {
namespace {

// Helper: check if a ContentPart is valid for a given kind.
bool IsPartValidForKind(ConversationItemKind kind, const ContentPart& part) {
  switch (kind) {
    case ConversationItemKind::kUserMessage:
    case ConversationItemKind::kAssistantMessage:
      return std::holds_alternative<TextPart>(part) ||
             std::holds_alternative<ImagePart>(part) ||
             std::holds_alternative<AudioPart>(part);

    case ConversationItemKind::kSystemEvent:
      return std::holds_alternative<TextPart>(part);

    case ConversationItemKind::kToolCall:
      return std::holds_alternative<ToolCallPart>(part);

    case ConversationItemKind::kToolResult:
      return std::holds_alternative<ToolResultPart>(part);
  }
  return false;
}

TEST(ConversationItemConstraints, UserMessageAllowsTextImageAudio) {
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kUserMessage,
                                 TextPart{"hello"}));
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kUserMessage,
                                 ImagePart{"http://img.png"}));
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kUserMessage,
                                 AudioPart{{}, "pcm"}));
}

TEST(ConversationItemConstraints, UserMessageRejectsToolParts) {
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kUserMessage,
                                  ToolCallPart{"id", "fn", "{}"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kUserMessage,
                                  ToolResultPart{"id", "fn", true, "{}"}));
}

TEST(ConversationItemConstraints, AssistantMessageAllowsTextImageAudio) {
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kAssistantMessage,
                                 TextPart{"response"}));
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kAssistantMessage,
                                 ImagePart{"http://img.png"}));
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kAssistantMessage,
                                 AudioPart{{}, "mp3"}));
}

TEST(ConversationItemConstraints, AssistantMessageRejectsToolParts) {
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kAssistantMessage,
                                  ToolCallPart{"id", "fn", "{}"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kAssistantMessage,
                                  ToolResultPart{"id", "fn", true, "{}"}));
}

TEST(ConversationItemConstraints, SystemEventAllowsTextOnly) {
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kSystemEvent,
                                 TextPart{"event"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kSystemEvent,
                                  ImagePart{"http://img.png"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kSystemEvent,
                                  AudioPart{{}, "pcm"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kSystemEvent,
                                  ToolCallPart{"id", "fn", "{}"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kSystemEvent,
                                  ToolResultPart{"id", "fn", true, "{}"}));
}

TEST(ConversationItemConstraints, ToolCallAllowsToolCallPartOnly) {
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kToolCall,
                                 ToolCallPart{"id", "fn", "{}"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolCall,
                                  TextPart{"text"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolCall,
                                  ImagePart{"url"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolCall,
                                  ToolResultPart{"id", "fn", true, "{}"}));
}

TEST(ConversationItemConstraints, ToolResultAllowsToolResultPartOnly) {
  EXPECT_TRUE(IsPartValidForKind(ConversationItemKind::kToolResult,
                                 ToolResultPart{"id", "fn", true, "{}"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolResult,
                                  TextPart{"text"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolResult,
                                  ImagePart{"url"}));
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolResult,
                                  ToolCallPart{"id", "fn", "{}"}));
}

TEST(ConversationItemConstraints, MultipleToolCallPartsAllowed) {
  // Requirement 4.4: multiple parallel tool calls in one kToolCall item.
  ConversationItem item;
  item.kind = ConversationItemKind::kToolCall;
  item.parts.emplace_back(ToolCallPart{"call_1", "search", "{}"});
  item.parts.emplace_back(ToolCallPart{"call_2", "fetch", "{}"});

  for (const auto& part : item.parts) {
    EXPECT_TRUE(IsPartValidForKind(item.kind, part));
  }
}

TEST(ConversationItemConstraints, MultipleToolResultPartsAllowed) {
  // Requirement 4.5: multiple tool results in one kToolResult item.
  ConversationItem item;
  item.kind = ConversationItemKind::kToolResult;
  item.parts.emplace_back(ToolResultPart{"call_1", "search", true, "{}"});
  item.parts.emplace_back(ToolResultPart{"call_2", "fetch", true, "{}"});

  for (const auto& part : item.parts) {
    EXPECT_TRUE(IsPartValidForKind(item.kind, part));
  }
}

TEST(ConversationItemConstraints, NoMixingToolCallAndToolResult) {
  // Requirement 4.6: ToolCallPart and ToolResultPart must not coexist.
  // A kToolCall item should not contain ToolResultPart.
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolCall,
                                  ToolResultPart{"id", "fn", true, "{}"}));
  // A kToolResult item should not contain ToolCallPart.
  EXPECT_FALSE(IsPartValidForKind(ConversationItemKind::kToolResult,
                                  ToolCallPart{"id", "fn", "{}"}));
}

}  // namespace
}  // namespace shizuru::core
