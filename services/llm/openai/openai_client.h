#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "interfaces/llm_client.h"
#include "llm/config.h"

namespace shizuru::services {

// OpenAI compatible LLM client implementing core::LlmClient.
// Uses HTTP + SSE streaming via libcurl.
class OpenAiClient : public core::LlmClient {
 public:
  explicit OpenAiClient(OpenAiConfig config);
  ~OpenAiClient() override;

  OpenAiClient(const OpenAiClient&) = delete;
  OpenAiClient& operator=(const OpenAiClient&) = delete;

  core::LlmResult Submit(const nlohmann::json& messages) override;

  core::LlmResult SubmitStreaming(const nlohmann::json& messages,
                                  core::StreamCallback on_token) override;

  void Cancel() override;

 private:
  static constexpr char MODULE_NAME[] = "LLM";

  std::string AuthHeader() const;
  std::string SchemeHost() const;

  OpenAiConfig config_;
  std::atomic<bool> cancel_requested_{false};
  std::mutex request_mutex_;
};

}  // namespace shizuru::services
