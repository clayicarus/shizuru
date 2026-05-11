#pragma once

// services/llm/openai/provider_render.h — Provider Render module.
//
// Projects InvokeBatch + history into OpenAI-compatible messages JSON.
// This is the terminal projection layer — internal models are NOT collapsed
// into provider payload format (requirement 11.1, 11.2, 11.3).
//
// Rendering rules:
//   kUserMessage → content array (TextPart/ImagePart/AudioPart)
//   kAssistantMessage → content string
//   kToolCall → tool_calls JSON array
//   kToolResult → role=tool message
//   Multi-actor group chat: inject <message> tags in TextPart (render layer)

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/conversation_item.h"
#include "core/invoke_batch.h"

namespace shizuru::services {

// Render a list of ConversationItems (history + batch) into OpenAI messages.
nlohmann::json RenderMessages(
    const std::vector<core::ConversationItem>& history,
    const core::InvokeBatch& batch,
    const std::string& system_instruction = "");

// Render a single ConversationItem into an OpenAI message JSON object.
nlohmann::json RenderItem(const core::ConversationItem& item,
                          bool multi_actor = false);

}  // namespace shizuru::services
