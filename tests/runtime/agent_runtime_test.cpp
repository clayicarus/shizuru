// Unit tests for AgentRuntime
// Uses Google Test + MinimalMockLlmServer

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "io/data_frame.h"
#include "runtime/agent_runtime.h"
#include "runtime/route_table.h"
#include "mock_io_device.h"

namespace shizuru::runtime {
namespace {

using testing::MockIoDevice;

// ---------------------------------------------------------------------------
// Minimal mock LLM HTTP server
// ---------------------------------------------------------------------------

class MockLlmServer {
 public:
  explicit MockLlmServer(bool use_tool_call = false)
      : call_count_(0), use_tool_call_(use_tool_call) {
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request&, httplib::Response& res) {
                   Handle(res);
                 });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
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
  void Handle(httplib::Response& res) {
    int call = call_count_++;
    nlohmann::json resp;
    resp["id"] = "mock";
    resp["object"] = "chat.completion";
    nlohmann::json usage;
    usage["prompt_tokens"] = 1;
    usage["completion_tokens"] = 1;
    resp["usage"] = usage;

    if (use_tool_call_ && call == 0) {
      nlohmann::json fn;
      fn["name"] = "noop";
      fn["arguments"] = "{}";
      nlohmann::json tc;
      tc["id"] = "call_0";
      tc["type"] = "function";
      tc["function"] = fn;
      nlohmann::json msg;
      msg["role"] = "assistant";
      msg["content"] = nullptr;
      msg["tool_calls"] = nlohmann::json::array({tc});
      nlohmann::json choice;
      choice["index"] = 0;
      choice["message"] = msg;
      choice["finish_reason"] = "tool_calls";
      resp["choices"] = nlohmann::json::array({choice});
    } else {
      nlohmann::json msg;
      msg["role"] = "assistant";
      msg["content"] = "hello from agent";
      nlohmann::json choice;
      choice["index"] = 0;
      choice["message"] = msg;
      choice["finish_reason"] = "stop";
      resp["choices"] = nlohmann::json::array({choice});
    }
    res.set_content(resp.dump(), "application/json");
  }

  httplib::Server server_;
  int port_;
  std::thread thread_;
  std::atomic<int> call_count_;
  bool use_tool_call_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

RuntimeConfig MakeConfig(const std::string& base_url) {
  RuntimeConfig cfg;
  cfg.controller.max_turns = 5;
  cfg.controller.max_retries = 0;
  cfg.controller.retry_base_delay = std::chrono::milliseconds(1);
  cfg.controller.wall_clock_timeout = std::chrono::seconds(10);
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

bool WaitFor(std::function<bool()> pred, int timeout_ms = 5000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

// ---------------------------------------------------------------------------
// Test 1: StartSession → SendMessage → output callback fires
// ---------------------------------------------------------------------------
TEST(AgentRuntimeTest, StartSessionSendMessageOutputCallbackFires) {
  MockLlmServer mock;
  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig(mock.BaseUrl()), tools);

  std::mutex mu;
  std::string received;
  runtime.OnOutput([&](const RuntimeOutput& out) {
    std::lock_guard<std::mutex> lock(mu);
    received = out.text;
  });

  runtime.StartSession();
  runtime.SendMessage("hello");

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !received.empty();
  });

  runtime.Shutdown();

  ASSERT_TRUE(got) << "Output callback never fired";
  std::lock_guard<std::mutex> lock(mu);
  EXPECT_FALSE(received.empty());
}

// ---------------------------------------------------------------------------
// Test 2: Shutdown stops all devices
// ---------------------------------------------------------------------------
TEST(AgentRuntimeTest, ShutdownStopsAllDevices) {
  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig("http://127.0.0.1:1"), tools);  // no real LLM needed

  auto dev1 = std::make_unique<MockIoDevice>("dev1");
  auto dev2 = std::make_unique<MockIoDevice>("dev2");
  MockIoDevice* d1 = dev1.get();
  MockIoDevice* d2 = dev2.get();

  runtime.RegisterDevice(std::move(dev1));
  runtime.RegisterDevice(std::move(dev2));

  // Manually start them (StartSession would need a real LLM).
  d1->Start();
  d2->Start();

  EXPECT_TRUE(d1->IsActive());
  EXPECT_TRUE(d2->IsActive());

  runtime.Shutdown();

  EXPECT_FALSE(d1->IsActive());
  EXPECT_FALSE(d2->IsActive());
}

// ---------------------------------------------------------------------------
// Test 3: GetState returns kTerminated when no session active
// ---------------------------------------------------------------------------
TEST(AgentRuntimeTest, GetStateTerminatedWithNoSession) {
  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig("http://127.0.0.1:1"), tools);

  EXPECT_EQ(runtime.GetState(), core::State::kTerminated);
}

// ---------------------------------------------------------------------------
// Test 4: RegisterDevice with duplicate ID throws std::invalid_argument
// ---------------------------------------------------------------------------
TEST(AgentRuntimeTest, RegisterDeviceDuplicateIdThrows) {
  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig("http://127.0.0.1:1"), tools);

  runtime.RegisterDevice(std::make_unique<MockIoDevice>("dup_id"));
  EXPECT_THROW(runtime.RegisterDevice(std::make_unique<MockIoDevice>("dup_id")),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Test 5: Frame emitted on port with no routes is silently discarded
// ---------------------------------------------------------------------------
TEST(AgentRuntimeTest, FrameWithNoRouteIsSilentlyDiscarded) {
  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig("http://127.0.0.1:1"), tools);

  auto src = std::make_unique<MockIoDevice>("src");
  MockIoDevice* src_ptr = src.get();
  runtime.RegisterDevice(std::move(src));
  src_ptr->Start();

  // No routes registered — emitting should not crash.
  io::DataFrame frame;
  frame.type = "text/plain";
  const std::string payload = "orphan";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());
  frame.timestamp = std::chrono::steady_clock::now();

  EXPECT_NO_THROW(src_ptr->EmitOutput("out_port", std::move(frame)));
}

// ---------------------------------------------------------------------------
// Test 6: DMA path delivers frame without control-plane check
// ---------------------------------------------------------------------------
TEST(AgentRuntimeTest, DmaPathDeliversFrameDirectly) {
  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig("http://127.0.0.1:1"), tools);

  auto src = std::make_unique<MockIoDevice>("src");
  auto dst = std::make_unique<MockIoDevice>("dst");
  MockIoDevice* src_ptr = src.get();
  MockIoDevice* dst_ptr = dst.get();

  runtime.RegisterDevice(std::move(src));
  runtime.RegisterDevice(std::move(dst));

  // DMA route: requires_control_plane = false
  runtime.AddRoute(PortAddress{"src", "audio_out"},
                   PortAddress{"dst", "audio_in"},
                   RouteOptions{.requires_control_plane = false});

  src_ptr->Start();
  dst_ptr->Start();

  io::DataFrame frame;
  frame.type = "audio/pcm";
  frame.payload = {0x01, 0x02, 0x03};
  frame.timestamp = std::chrono::steady_clock::now();

  src_ptr->EmitOutput("audio_out", frame);

  // Frame must arrive at dst immediately (DMA — no gating).
  ASSERT_EQ(dst_ptr->ReceivedCount(), 1u);
  const auto& received = dst_ptr->ReceivedFrames();
  EXPECT_EQ(received[0].first, "audio_in");
  EXPECT_EQ(received[0].second.type, "audio/pcm");
  EXPECT_EQ(received[0].second.payload, frame.payload);
}

}  // namespace
}  // namespace shizuru::runtime
