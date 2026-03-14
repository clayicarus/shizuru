#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/types.h"
#include "controller/types.h"
#include "interfaces/llm_client.h"
#include "llm/config.h"

namespace shizuru::services {

// Serialize a ContextWindow + tool definitions into an OpenAI chat completion
// request body (JSON string).
std::string SerializeRequest(const core::ContextWindow& context,
                             const OpenAiConfig& config);

// Serialize tool definitions into the OpenAI "tools" JSON array.
nlohmann::json SerializeTools(const std::vector<ToolDefinition>& tools);

// Parse a complete (non-streaming) OpenAI chat completion response.
// Throws std::runtime_error on malformed JSON.
core::LlmResult ParseResponse(const std::string& response_body);

// Parse a single SSE data line from a streaming response.
// Returns true if the chunk was parsed successfully.
// Sets is_done to true when the stream is finished ([DONE]).
// Appends content delta to accumulated_content.
// Populates tool_call fields incrementally.
bool ParseStreamChunk(const std::string& data_line,
                      std::string& accumulated_content,
                      nlohmann::json& accumulated_tool_calls,
                      core::LlmResult& result,
                      bool& is_done);

}  // namespace shizuru::services
