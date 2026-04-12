#include "llm/openai/json_parser.h"

#include <random>
#include <stdexcept>

namespace shizuru::services {

// Generate a unique tool call ID when the API doesn't provide one.
std::string GenerateToolCallId() {
  static thread_local std::mt19937 rng(std::random_device{}());
  static constexpr char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string id = "call_";
  for (int i = 0; i < 12; ++i) {
    id += kChars[rng() % (sizeof(kChars) - 1)];
  }
  return id;
}

namespace {

// Map ContextMessage role to OpenAI message object.
nlohmann::json MessageToJson(const core::ContextMessage& msg) {
  nlohmann::json j;
  j["role"] = msg.role;

  if (!msg.tool_calls_json.empty()) {
    // Assistant tool call message: content must be null, tool_calls is the array.
    j["content"] = nullptr;
    try {
      auto tool_calls = nlohmann::json::parse(msg.tool_calls_json);
      if (tool_calls.is_array()) {
        for (auto& tc : tool_calls) {
          if (tc.contains("function") && tc["function"].contains("arguments") &&
              !tc["function"]["arguments"].is_string()) {
            tc["function"]["arguments"] =
                tc["function"]["arguments"].dump();
          }
        }
      }
      j["tool_calls"] = std::move(tool_calls);
    } catch (...) {
      j["tool_calls"] = nlohmann::json::array();
    }
  } else {
    j["content"] = msg.content;
  }

  if (!msg.tool_call_id.empty()) {
    j["tool_call_id"] = msg.tool_call_id;
  }
  if (!msg.name.empty()) {
    j["name"] = msg.name;
  }
  return j;
}

// Parse an ActionCandidate from the first choice in a chat completion response.
core::ActionCandidate ParseCandidate(const nlohmann::json& choice) {
  core::ActionCandidate ac;

  auto& message = choice.at("message");

  // Check for tool_calls first (function calling).
  if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
    ac.type = core::ActionType::kToolCall;

    for (const auto& tc : message["tool_calls"]) {
      core::ToolCall call;
      if (tc.contains("id") && !tc["id"].get<std::string>().empty()) {
        call.id = tc["id"].get<std::string>();
      } else {
        call.id = GenerateToolCallId();
      }
      call.name = tc.at("function").at("name").get<std::string>();
      call.arguments = tc.at("function").at("arguments").get<std::string>();
      ac.tool_calls.push_back(std::move(call));
    }

    return ac;
  }

  // Check for content (text response).
  std::string content;
  if (message.contains("content") && !message["content"].is_null()) {
    content = message["content"].get<std::string>();
  }

  if (!content.empty()) {
    ac.type = core::ActionType::kResponse;
    ac.response_text = content;
    return ac;
  }

  // No tool_calls and no content → continue.
  ac.type = core::ActionType::kContinue;
  return ac;
}

}  // namespace

std::string SerializeRequest(const core::ContextWindow& context,
                             const OpenAiConfig& config) {
  nlohmann::json body;
  body["model"] = config.model;
  body["temperature"] = config.temperature;
  body["max_tokens"] = config.max_tokens;
  body["stream"] = false;

  // Messages array.
  nlohmann::json messages = nlohmann::json::array();
  for (const auto& msg : context.messages) {
    messages.push_back(MessageToJson(msg));
  }
  body["messages"] = std::move(messages);

  // Tools (function calling).
  if (!config.tools.empty()) {
    body["tools"] = SerializeTools(config.tools);
  }

  return body.dump();
}

nlohmann::json SerializeTools(const std::vector<ToolDefinition>& tools) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& tool : tools) {
    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json required_params = nlohmann::json::array();

    for (const auto& param : tool.parameters) {
      nlohmann::json prop;
      prop["type"] = param.type;
      prop["description"] = param.description;
      properties[param.name] = std::move(prop);
      if (param.required) {
        required_params.push_back(param.name);
      }
    }

    nlohmann::json func;
    func["name"] = tool.name;
    func["description"] = tool.description;
    func["parameters"] = {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required_params)},
    };

    nlohmann::json entry;
    entry["type"] = "function";
    entry["function"] = std::move(func);
    arr.push_back(std::move(entry));
  }
  return arr;
}

core::LlmResult ParseResponse(const std::string& response_body) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(response_body);
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("Failed to parse LLM response: ") +
                             e.what());
  }

  // Check for API error.
  if (j.contains("error")) {
    std::string msg = "LLM API error";
    if (j["error"].contains("message")) {
      msg = j["error"]["message"].get<std::string>();
    }
    throw std::runtime_error(msg);
  }

  if (!j.contains("choices") || j["choices"].empty()) {
    throw std::runtime_error("LLM response has no choices");
  }

  core::LlmResult result;
  result.candidate = ParseCandidate(j["choices"][0]);

  // Token usage.
  if (j.contains("usage")) {
    auto& usage = j["usage"];
    if (usage.contains("prompt_tokens")) {
      result.prompt_tokens = usage["prompt_tokens"].get<int>();
    }
    if (usage.contains("completion_tokens")) {
      result.completion_tokens = usage["completion_tokens"].get<int>();
    }
  }

  return result;
}

bool ParseStreamChunk(const std::string& data_line,
                      std::string& accumulated_content,
                      nlohmann::json& accumulated_tool_calls,
                      core::LlmResult& result,
                      bool& is_done) {
  is_done = false;

  // Strip "data: " prefix if present.
  std::string data = data_line;
  if (data.substr(0, 6) == "data: ") {
    data = data.substr(6);
  }

  // Trim whitespace.
  while (!data.empty() && (data.back() == '\n' || data.back() == '\r' ||
                           data.back() == ' ')) {
    data.pop_back();
  }

  if (data.empty()) {
    return false;
  }

  if (data == "[DONE]") {
    is_done = true;

    // Build final ActionCandidate from accumulated data.
    if (!accumulated_tool_calls.empty() &&
        accumulated_tool_calls.is_array() &&
        !accumulated_tool_calls[0].empty()) {
      result.candidate.type = core::ActionType::kToolCall;

      for (const auto& tc : accumulated_tool_calls) {
        if (tc.empty()) continue;
        core::ToolCall call;
        if (tc.contains("id") && !tc["id"].get<std::string>().empty()) {
          call.id = tc["id"].get<std::string>();
        } else {
          call.id = GenerateToolCallId();
        }
        if (tc.contains("function")) {
          if (tc["function"].contains("name")) {
            call.name = tc["function"]["name"].get<std::string>();
          }
          if (tc["function"].contains("arguments")) {
            call.arguments = tc["function"]["arguments"].get<std::string>();
          }
        }
        result.candidate.tool_calls.push_back(std::move(call));
      }
    } else if (!accumulated_content.empty()) {
      result.candidate.type = core::ActionType::kResponse;
      result.candidate.response_text = accumulated_content;
    } else {
      result.candidate.type = core::ActionType::kContinue;
    }

    return true;
  }

  nlohmann::json chunk;
  try {
    chunk = nlohmann::json::parse(data);
  } catch (...) {
    return false;  // Skip malformed chunks.
  }

  if (!chunk.contains("choices") || chunk["choices"].empty()) {
    // May contain usage info at the end.
    if (chunk.contains("usage")) {
      auto& usage = chunk["usage"];
      if (usage.contains("prompt_tokens")) {
        result.prompt_tokens = usage["prompt_tokens"].get<int>();
      }
      if (usage.contains("completion_tokens")) {
        result.completion_tokens = usage["completion_tokens"].get<int>();
      }
    }
    return true;
  }

  auto& delta = chunk["choices"][0];
  if (!delta.contains("delta")) {
    return true;
  }

  auto& d = delta["delta"];

  // Accumulate content delta.
  if (d.contains("content") && !d["content"].is_null()) {
    accumulated_content += d["content"].get<std::string>();
  }

  // Accumulate tool_calls delta.
  if (d.contains("tool_calls")) {
    for (auto& tc_delta : d["tool_calls"]) {
      int index = 0;
      if (tc_delta.contains("index")) {
        index = tc_delta["index"].get<int>();
      }

      // Ensure the array is large enough.
      while (accumulated_tool_calls.size() <=
             static_cast<size_t>(index)) {
        accumulated_tool_calls.push_back(nlohmann::json::object());
      }

      auto& acc = accumulated_tool_calls[index];

      if (tc_delta.contains("id")) {
        acc["id"] = tc_delta["id"].get<std::string>();
      }
      if (tc_delta.contains("type")) {
        acc["type"] = tc_delta["type"].get<std::string>();
      }
      if (tc_delta.contains("function")) {
        if (!acc.contains("function")) {
          acc["function"] = nlohmann::json::object();
        }
        if (tc_delta["function"].contains("name")) {
          if (!acc["function"].contains("name")) {
            acc["function"]["name"] = "";
          }
          acc["function"]["name"] = acc["function"]["name"].get<std::string>() +
                                    tc_delta["function"]["name"].get<std::string>();
        }
        if (tc_delta["function"].contains("arguments")) {
          if (!acc["function"].contains("arguments")) {
            acc["function"]["arguments"] = "";
          }
          acc["function"]["arguments"] =
              acc["function"]["arguments"].get<std::string>() +
              tc_delta["function"]["arguments"].get<std::string>();
        }
      }
    }
  }

  return true;
}

}  // namespace shizuru::services
