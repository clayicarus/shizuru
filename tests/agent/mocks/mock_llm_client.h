#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "interfaces/llm_client.h"

namespace shizuru::core::testing {

// Hand-written mock for LlmClient.
// Set submit_fn to control Submit/SubmitStreaming behavior in tests.
// All calls are recorded in submit_calls for verification.
class MockLlmClient : public LlmClient {
 public:
  // Configurable behavior for Submit calls.
  std::function<LlmResult(const ContextWindow&)> submit_fn;
  std::function<LlmResult(const ContextWindow&, StreamCallback)>
      submit_streaming_fn;

  // Recorded Submit calls (each is the ContextWindow that was passed).
  std::vector<ContextWindow> submit_calls;

  // Recorded SubmitStreaming calls.
  std::vector<ContextWindow> submit_streaming_calls;

  // Number of Cancel() calls received.
  int cancel_count = 0;

  LlmResult Submit(const ContextWindow& context) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      submit_calls.push_back(context);
    }
    if (submit_fn) {
      return submit_fn(context);
    }
    // Default: return empty result
    return LlmResult{};
  }

  LlmResult SubmitStreaming(const ContextWindow& context,
                            StreamCallback on_token) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      submit_streaming_calls.push_back(context);
    }
    if (submit_streaming_fn) {
      return submit_streaming_fn(context, std::move(on_token));
    }
    if (submit_fn) {
      return submit_fn(context);
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

  // Blocks until Cancel() is called or timeout_ms elapses. Returns true if
  // cancelled. Useful in submit_fn lambdas to simulate a cancellable LLM call.
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
