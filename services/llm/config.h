#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace shizuru::services {

// Tool parameter schema for OpenAI function calling.
struct ToolParameter {
  std::string name;
  std::string type;         // "string", "integer", "boolean", "object", "array"
  std::string description;
  bool required = false;
};

// Tool definition for OpenAI function calling.
struct ToolDefinition {
  std::string name;
  std::string description;
  std::vector<ToolParameter> parameters;
  std::string required_capability;  // Maps to ActionCandidate::required_capability
};

// Configuration for the OpenAI compatible LLM client.
struct OpenAiConfig {
  std::string api_key;
  std::string base_url = "https://api.openai.com";
  std::string model = "gpt-4o";
  std::string api_path = "/v1/chat/completions";

  double temperature = 0.7;
  int max_tokens = 4096;

  std::chrono::seconds connect_timeout{10};
  std::chrono::seconds read_timeout{60};

  // Tool definitions for function calling.
  std::vector<ToolDefinition> tools;

  // Enable extended thinking / chain-of-thought mode.
  // When true, the request includes "enable_thinking": true.
  bool enable_thinking = false;
};

}  // namespace shizuru::services
