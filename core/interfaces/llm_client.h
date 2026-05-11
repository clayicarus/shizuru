#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "controller/types.h"

namespace shizuru::core {

// Callback for streaming tokens from the LLM.
using StreamCallback = std::function<void(const std::string& token)>;

// Result of an LLM inference call.
struct LlmResult {
  ActionCandidate candidate;
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

// Tool definition for LLM function calling.
struct ToolDefinition {
  std::string name;
  std::string description;
  nlohmann::json parameters;  // JSON Schema for the tool's parameters.
};

// Abstract interface for LLM service clients.
// Concrete implementations (OpenAI, Anthropic, local, etc.) live outside core/.
//
// The input is a pre-rendered JSON messages array (produced by provider_render).
// This decouples the LLM client from internal data models (requirement 11.2).
class LlmClient {
 public:
  virtual ~LlmClient() = default;

  // Submit pre-rendered messages JSON and receive an action candidate.
  // Blocks until the full response is available.
  virtual LlmResult Submit(const nlohmann::json& messages) = 0;

  // Submit with streaming callback for incremental token delivery.
  virtual LlmResult SubmitStreaming(const nlohmann::json& messages,
                                    StreamCallback on_token) = 0;

  // Request cancellation of an in-progress call.
  virtual void Cancel() = 0;
};

}  // namespace shizuru::core
