#pragma once

#include <atomic>
#include <mutex>
#include <string>

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

  core::LlmResult Submit(const core::ContextWindow& context) override;

  core::LlmResult SubmitStreaming(const core::ContextWindow& context,
                                  core::StreamCallback on_token) override;

  void Cancel() override;

 private:
  static constexpr char MODULE_NAME[] = "LLM";

  // Build the Authorization header value.
  std::string AuthHeader() const;

  // Return the base URL (scheme + host).
  std::string SchemeHost() const;

  OpenAiConfig config_;
  std::atomic<bool> cancel_requested_{false};
  std::mutex request_mutex_;
};

}  // namespace shizuru::services
