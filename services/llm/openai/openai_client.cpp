#include "llm/openai/openai_client.h"

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "llm/openai/json_parser.h"
#include "async_logger.h"
#include "services/utils/curl_helper.h"

namespace shizuru::services {

OpenAiClient::OpenAiClient(OpenAiConfig config)
    : config_(std::move(config)) {}

OpenAiClient::~OpenAiClient() = default;

std::string OpenAiClient::AuthHeader() const {
  return "Bearer " + config_.api_key;
}

std::string OpenAiClient::SchemeHost() const {
  return config_.base_url;
}

core::LlmResult OpenAiClient::Submit(const core::ContextWindow& context) {
  std::lock_guard<std::mutex> lock(request_mutex_);
  cancel_requested_.store(false);

  std::string body = SerializeRequest(context, config_);
  std::string url = SchemeHost() + config_.api_path;

  LOG_DEBUG("[{}] Submit to {}", MODULE_NAME, url);
  LOG_DEBUG("[{}] Payload: {}", MODULE_NAME, body);

  auto res = CurlPost(
      url,
      {"Authorization: " + AuthHeader(),
       "Content-Type: application/json"},
      body,
      config_.connect_timeout,
      config_.read_timeout);

  if (res.status_code != 200) {
    LOG_WARN("[{}] Submit status {}: {}", MODULE_NAME, res.status_code, res.body);
    throw std::runtime_error("LLM API returned status " +
                             std::to_string(res.status_code) + ": " + res.body);
  }

  LOG_DEBUG("[{}] Submit response: {}", MODULE_NAME, res.body);

  return ParseResponse(res.body);
}

core::LlmResult OpenAiClient::SubmitStreaming(
    const core::ContextWindow& context, core::StreamCallback on_token) {
  std::lock_guard<std::mutex> lock(request_mutex_);
  cancel_requested_.store(false);

  // Build streaming request body.
  nlohmann::json body_json = nlohmann::json::parse(
      SerializeRequest(context, config_));
  body_json["stream"] = true;
  body_json["stream_options"] = {{"include_usage", true}};
  std::string body = body_json.dump();
  std::string url = SchemeHost() + config_.api_path;

  LOG_DEBUG("[{}] SubmitStreaming to {}", MODULE_NAME, url);
  LOG_DEBUG("[{}] Payload: {}", MODULE_NAME, body);

  core::LlmResult result;
  std::string accumulated_content;
  nlohmann::json accumulated_tool_calls = nlohmann::json::array();
  std::string line_buffer;
  bool stream_done = false;
  size_t prev_content_len = 0;

  auto on_data = [&](const char* data, size_t len) -> bool {
    if (cancel_requested_.load()) { return false; }

    line_buffer.append(data, len);

    // Process complete lines (SSE format).
    std::string::size_type pos;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
      std::string line = line_buffer.substr(0, pos);
      line_buffer.erase(0, pos + 1);

      if (line.empty() || line == "\r") {
        continue;
      }

      bool is_done = false;
      if (ParseStreamChunk(line, accumulated_content,
                           accumulated_tool_calls, result, is_done)) {
        if (is_done) {
          stream_done = true;
          return true;
        }

        // Deliver content delta via callback.
        if (on_token && accumulated_content.size() > prev_content_len) {
          std::string delta =
              accumulated_content.substr(prev_content_len);
          prev_content_len = accumulated_content.size();
          on_token(delta);
        }
      }
    }

    return true;
  };

  long status = CurlPostStreaming(
      url,
      {"Authorization: " + AuthHeader(),
       "Content-Type: application/json"},
      body,
      config_.connect_timeout,
      config_.read_timeout,
      on_data,
      cancel_requested_);

  if (cancel_requested_.load()) {
    LOG_WARN("[{}] SubmitStreaming cancelled by user", MODULE_NAME);
    throw std::runtime_error("Request cancelled");
  }

  if (status != 200) {
    LOG_WARN("[{}] SubmitStreaming status {}", MODULE_NAME, status);
    throw std::runtime_error("LLM API returned status " +
                             std::to_string(status));
  }

  // If stream didn't produce a [DONE] marker, build result from accumulated.
  if (!stream_done) {
    if (!accumulated_tool_calls.empty() && accumulated_tool_calls.is_array() &&
        !accumulated_tool_calls[0].empty()) {
      result.candidate.type = core::ActionType::kToolCall;

      for (const auto& tc : accumulated_tool_calls) {
        if (tc.empty()) { continue; }
        core::ToolCall call;
        if (tc.contains("id")) {
          call.id = tc["id"].get<std::string>();
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

      if (!result.candidate.tool_calls.empty()) {
        result.candidate.action_name = result.candidate.tool_calls[0].name;
        result.candidate.arguments = result.candidate.tool_calls[0].arguments;
        result.candidate.response_text = result.candidate.tool_calls[0].id;
      }
    } else if (!accumulated_content.empty()) {
      result.candidate.type = core::ActionType::kResponse;
      result.candidate.response_text = accumulated_content;
    } else {
      result.candidate.type = core::ActionType::kContinue;
    }
  }

  LOG_DEBUG("[{}] SubmitStreaming result: type={}, text=\"{}\"", MODULE_NAME,
            static_cast<int>(result.candidate.type),
            result.candidate.type == core::ActionType::kToolCall
                ? result.candidate.action_name
                : result.candidate.response_text);

  return result;
}

void OpenAiClient::Cancel() {
  cancel_requested_.store(true);
}

}  // namespace shizuru::services
