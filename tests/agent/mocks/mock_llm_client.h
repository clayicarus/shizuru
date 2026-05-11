#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "interfaces/llm_client.h"

namespace shizuru::core::testing {

// Hand-written mock for LlmClient.
// Set submit_fn to control Submit/SubmitStreaming behavior in tests.
class MockLlmClient : public LlmClient {
 public:
  // Configurable behavior for Submit calls.
  std::function<LlmResult(const nlohmann::json&)> submit_fn;
  std::function<LlmResult(const nlohmann::json&, StreamCallback)>
      submit_streaming_fn;

  // Recorded Submit calls (each is the messages JSON that was passed).
  std::vector<nlohmann::json> submit_calls;

  // Recorded SubmitStreaming calls.
  std::vector<nlohmann::json> submit_streaming_calls;

  // Number of Cancel() calls received.
  int cancel_count = 0;

  LlmResult Submit(const nlohmann::json& messages) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      submit_calls.push_back(messages);
    }
    if (submit_fn) {
      return submit_fn(messages);
    }
    return LlmResult{};
  }

  LlmResult SubmitStreaming(const nlohmann::json& messages,
                            StreamCallback on_token) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      submit_streaming_calls.push_back(messages);
    }
    if (submit_streaming_fn) {
      return submit_streaming_fn(messages, std::move(on_token));
    }
    if (submit_fn) {
      return submit_fn(messages);
    }
    return LlmResult{};
  }

  void Cancel() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++cancel_count;
    }
    cancel_cv_.notify_all();
  }

  bool WaitForCancel(int timeout_ms = 500) {
    std::unique_lock<std::mutex> lock(mu_);
    return cancel_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this] { return cancel_count > 0; });
  }

 private:
  std::mutex mu_;
  std::condition_variable cancel_cv_;
};

}  // namespace shizuru::core::testing
