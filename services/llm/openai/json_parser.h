#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "controller/types.h"
#include "interfaces/llm_client.h"
#include "llm/config.h"

namespace shizuru::services {

// Serialize pre-rendered messages JSON + tool definitions into an OpenAI chat
// completion request body (JSON string).
std::string SerializeRequest(const nlohmann::json& messages,
                             const OpenAiConfig& config);

// Serialize tool definitions into the OpenAI "tools" JSON array.
nlohmann::json SerializeTools(const std::vector<ToolDefinition>& tools);

// Parse a complete (non-streaming) OpenAI chat completion response.
// Throws std::runtime_error on malformed JSON.
core::LlmResult ParseResponse(const std::string& response_body);

// Parse a single SSE data line from a streaming response.
bool ParseStreamChunk(const std::string& data_line,
                      std::string& accumulated_content,
                      nlohmann::json& accumulated_tool_calls,
                      core::LlmResult& result,
                      bool& is_done);

// Generate a unique tool call ID when the API doesn't provide one.
std::string GenerateToolCallId();

}  // namespace shizuru::services
