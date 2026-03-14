#include "llm/openai_client.h"

#include <sstream>
#include <stdexcept>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "llm/json_parser.h"

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

  httplib::Client cli(SchemeHost());
  cli.set_connection_timeout(config_.connect_timeout);
  cli.set_read_timeout(config_.read_timeout);

  httplib::Headers headers = {
      {"Authorization", AuthHeader()},
      {"Content-Type", "application/json"},
  };

  auto res = cli.Post(config_.api_path, headers, body, "application/json");

  if (!res) {
    throw std::runtime_error("HTTP request failed: " +
                             httplib::to_string(res.error()));
  }

  if (res->status != 200) {
    throw std::runtime_error("LLM API returned status " +
                             std::to_string(res->status) + ": " + res->body);
  }

  return ParseResponse(res->body);
}

core::LlmResult OpenAiClient::SubmitStreaming(
    const core::ContextWindow& context, core::StreamCallback on_token) {
  std::lock_guard<std::mutex> lock(request_mutex_);
  cancel_requested_.store(false);

  // Build streaming request body.
  nlohmann::json body_json = nlohmann::json::parse(
      SerializeRequest(context, config_));
  body_json["stream"] = true;
  // Request usage stats in the final chunk.
  body_json["stream_options"] = {{"include_usage", true}};
  std::string body = body_json.dump();

  httplib::Client cli(SchemeHost());
  cli.set_connection_timeout(config_.connect_timeout);
  cli.set_read_timeout(config_.read_timeout);

  httplib::Headers headers = {
      {"Authorization", AuthHeader()},
      {"Content-Type", "application/json"},
  };

  core::LlmResult result;
  std::string accumulated_content;
  nlohmann::json accumulated_tool_calls = nlohmann::json::array();
  std::string line_buffer;
  bool stream_done = false;
  size_t prev_content_len = 0;

  // Construct a POST request with a content_receiver for streaming.
  // cpp-httplib only provides ContentReceiver overloads for Get, so we
  // build the Request manually and call send().
  httplib::Request req;
  req.method = "POST";
  req.path = config_.api_path;
  req.headers = headers;
  req.body = body;
  req.set_header("Content-Type", "application/json");
  req.content_receiver =
      [&](const char* data, size_t len, uint64_t /*offset*/,
          uint64_t /*total_length*/) -> bool {
        if (cancel_requested_.load()) {
          return false;  // Abort the request.
        }

        line_buffer.append(data, len);

        // Process complete lines.
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

        return true;  // Continue reading.
      };

  auto res = cli.send(req);

  if (cancel_requested_.load()) {
    throw std::runtime_error("Request cancelled");
  }

  if (!res) {
    throw std::runtime_error("HTTP streaming request failed: " +
                             httplib::to_string(res.error()));
  }

  if (res->status != 200) {
    throw std::runtime_error("LLM API returned status " +
                             std::to_string(res->status));
  }

  // If stream didn't produce a [DONE] marker, build result from accumulated.
  if (!stream_done) {
    if (!accumulated_tool_calls.empty() && accumulated_tool_calls.is_array() &&
        !accumulated_tool_calls[0].empty()) {
      auto& tc = accumulated_tool_calls[0];
      result.candidate.type = core::ActionType::kToolCall;
      if (tc.contains("function")) {
        if (tc["function"].contains("name")) {
          result.candidate.action_name =
              tc["function"]["name"].get<std::string>();
        }
        if (tc["function"].contains("arguments")) {
          result.candidate.arguments =
              tc["function"]["arguments"].get<std::string>();
        }
      }
    } else if (!accumulated_content.empty()) {
      result.candidate.type = core::ActionType::kResponse;
      result.candidate.response_text = accumulated_content;
    } else {
      result.candidate.type = core::ActionType::kContinue;
    }
  }

  return result;
}

void OpenAiClient::Cancel() {
  cancel_requested_.store(true);
}

}  // namespace shizuru::services
