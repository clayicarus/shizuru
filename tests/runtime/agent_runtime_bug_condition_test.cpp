// Bug condition exploration test for AgentRuntime
// This test is EXPECTED TO FAIL on unfixed code — failure confirms the bug exists.
// DO NOT fix the test or the code when it fails.
//
// Bug 3: Concurrent SendMessage/Shutdown race — SendMessage() reads core_device_
// without holding devices_mutex_, so Shutdown() can null it out between the
// null check and the OnInput() call, causing a use-after-free or crash.
//
// **Validates: Requirements 1.5, 1.6, 1.7**

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "runtime/agent_runtime.h"

namespace shizuru::runtime {
namespace {

// Minimal mock LLM HTTP server — returns a simple response for every request.
class MockLlmServer {
 public:
  MockLlmServer() {
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request&, httplib::Response& res) {
                   HandleRequest(res);
                 });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    // Wait for server to be ready.
    for (int i = 0; i < 100; ++i) {
      httplib::Client cli("http://127.0.0.1:" + std::to_string(port_));
      cli.set_connection_timeout(std::chrono::milliseconds(50));
      if (cli.Get("/healthz")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ~MockLlmServer() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

  std::string BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  void HandleRequest(httplib::Response& res) {
    nlohmann::json resp;
    resp["id"] = "mock";
    resp["object"] = "chat.completion";
    nlohmann::json usage;
    usage["prompt_tokens"] = 1;
    usage["completion_tokens"] = 1;
    resp["usage"] = usage;
    nlohmann::json msg;
    msg["role"] = "assistant";
    msg["content"] = "ok";
    nlohmann::json choice;
    choice["index"] = 0;
    choice["message"] = msg;
    choice["finish_reason"] = "stop";
    resp["choices"] = nlohmann::json::array({choice});
    res.set_content(resp.dump(), "application/json");
  }

  httplib::Server server_;
  int port_;
  std::thread thread_;
};

RuntimeConfig MakeConfig(const std::string& base_url) {
  RuntimeConfig cfg;
  cfg.controller.max_turns = 5;
  cfg.controller.max_retries = 0;
  cfg.controller.retry_base_delay = std::chrono::milliseconds(1);
  cfg.controller.turn_timeout = std::chrono::seconds(10);
  cfg.controller.token_budget = 100000;
  cfg.controller.action_count_limit = 10;
  cfg.context.max_context_tokens = 100000;
  cfg.llm.base_url = base_url;
  cfg.llm.api_key = "mock";
  cfg.llm.model = "mock";
  cfg.llm.connect_timeout = std::chrono::seconds(5);
  cfg.llm.read_timeout = std::chrono::seconds(10);
  return cfg;
}

// ---------------------------------------------------------------------------
// Bug 3 — Concurrent SendMessage/Shutdown Race
// ---------------------------------------------------------------------------
// On unfixed code, SendMessage() reads core_device_ without holding
// devices_mutex_. A concurrent Shutdown() can set core_device_ to nullptr
// between the null check and the OnInput() call, causing a use-after-free
// or null pointer dereference.
//
// This test creates an AgentRuntime, starts a session, then launches 10
// threads that alternate between SendMessage() and Shutdown() calls
// concurrently. On unfixed code, this triggers a data race that crashes
// or is detected by ThreadSanitizer.
//
// **Validates: Requirements 1.5, 1.6, 1.7**
// ---------------------------------------------------------------------------
TEST(AgentRuntimeBugCondition, Bug3_ConcurrentSendMessageShutdown_DataRace) {
  MockLlmServer mock;
  services::ToolRegistry tools;

  auto cfg = MakeConfig(mock.BaseUrl());
  AgentRuntime runtime(cfg, tools);

  // Start a session so core_device_ is non-null.
  runtime.StartSession();

  // Give the session a moment to fully initialize.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Launch 10 threads: odd threads call SendMessage, even threads call Shutdown.
  // This creates a race between SendMessage reading core_device_ and Shutdown
  // nulling it out.
  constexpr int kNumThreads = 10;
  std::vector<std::thread> threads;
  std::atomic<int> crash_count{0};

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&, i] {
      try {
        if (i % 2 == 0) {
          runtime.SendMessage("msg_" + std::to_string(i));
        } else {
          runtime.Shutdown();
        }
      } catch (...) {
        crash_count.fetch_add(1);
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Clean up — Shutdown() may have already been called by one of the threads.
  runtime.Shutdown();

  // On unfixed code, this test either:
  // 1. Crashes with a segfault (use-after-free on core_device_)
  // 2. Reports a data race under ThreadSanitizer
  // 3. Exhibits undefined behavior
  //
  // If we reach here without crashing, the race may not have manifested in
  // this particular run (timing-dependent). Under TSan, the data race will
  // be reliably detected.
  EXPECT_EQ(crash_count.load(), 0)
      << "Bug 3: Caught " << crash_count.load()
      << " exceptions during concurrent SendMessage/Shutdown";

  // The real confirmation of Bug 3 is via ThreadSanitizer or AddressSanitizer.
  // This test documents the race condition pattern.
  SUCCEED() << "Test completed — run under TSan to confirm data race on unfixed code";
}

}  // namespace
}  // namespace shizuru::runtime
