// Integration tests for AgentRuntime — full pipeline, interrupt path, tool call round-trip.
// Uses MockLlmServer (httplib) and MockIoDevice to exercise the assembled system
// through AgentRuntime's public API.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "io/data_frame.h"
#include "runtime/agent_runtime.h"
#include "policy/types.h"
#include "mock_io_device.h"

namespace shizuru::runtime {
namespace {

using testing::MockIoDevice;

// ---------------------------------------------------------------------------
// Mock LLM HTTP server — configurable per-test via handler lambda
// ---------------------------------------------------------------------------

class MockLlmServer {
 public:
  using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;

  explicit MockLlmServer(Handler handler) : handler_(std::move(handler)) {
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   handler_(req, res);
                 });
    // Health check endpoint for readiness polling.
    server_.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
      res.set_content("ok", "text/plain");
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
  httplib::Server server_;
  int port_ = 0;
  std::thread thread_;
  Handler handler_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

RuntimeConfig MakeConfig(const std::string& base_url, bool streaming = false) {
  RuntimeConfig cfg;
  cfg.controller.max_turns = 10;
  cfg.controller.max_retries = 0;
  cfg.controller.retry_base_delay = std::chrono::milliseconds(1);
  cfg.controller.turn_timeout = std::chrono::seconds(15);
  cfg.controller.token_budget = 100000;
  cfg.controller.action_count_limit = 20;
  cfg.controller.use_streaming = streaming;
  cfg.controller.tool_call_timeout = std::chrono::seconds(10);
  cfg.context.max_context_tokens = 100000;
  cfg.llm.base_url = base_url;
  cfg.llm.api_key = "mock";
  cfg.llm.model = "mock";
  cfg.llm.connect_timeout = std::chrono::seconds(5);
  cfg.llm.read_timeout = std::chrono::seconds(10);
  return cfg;
}

// Build a standard non-streaming chat completion JSON response.
std::string MakeChatResponse(const std::string& content) {
  nlohmann::json msg;
  msg["role"] = "assistant";
  msg["content"] = content;
  nlohmann::json choice;
  choice["index"] = 0;
  choice["message"] = msg;
  choice["finish_reason"] = "stop";
  nlohmann::json usage;
  usage["prompt_tokens"] = 5;
  usage["completion_tokens"] = 5;
  nlohmann::json resp;
  resp["id"] = "mock";
  resp["object"] = "chat.completion";
  resp["choices"] = nlohmann::json::array({choice});
  resp["usage"] = usage;
  return resp.dump();
}

// Build a tool_calls chat completion JSON response.
std::string MakeToolCallResponse(const std::string& tool_name,
                                 const std::string& tool_id,
                                 const std::string& arguments) {
  nlohmann::json fn;
  fn["name"] = tool_name;
  fn["arguments"] = arguments;
  nlohmann::json tc;
  tc["id"] = tool_id;
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
  nlohmann::json usage;
  usage["prompt_tokens"] = 5;
  usage["completion_tokens"] = 5;
  nlohmann::json resp;
  resp["id"] = "mock";
  resp["object"] = "chat.completion";
  resp["choices"] = nlohmann::json::array({choice});
  resp["usage"] = usage;
  return resp.dump();
}

// Build SSE streaming response body for a simple text response.
std::string MakeStreamingResponse(const std::string& content) {
  std::string sse;
  // First chunk: role
  nlohmann::json chunk1;
  chunk1["id"] = "mock";
  chunk1["object"] = "chat.completion.chunk";
  nlohmann::json delta1;
  delta1["role"] = "assistant";
  delta1["content"] = "";
  nlohmann::json c1;
  c1["index"] = 0;
  c1["delta"] = delta1;
  chunk1["choices"] = nlohmann::json::array({c1});
  sse += "data: " + chunk1.dump() + "\n\n";

  // Content chunks — one per character for simplicity.
  for (char ch : content) {
    nlohmann::json chunk;
    chunk["id"] = "mock";
    chunk["object"] = "chat.completion.chunk";
    nlohmann::json delta;
    delta["content"] = std::string(1, ch);
    nlohmann::json c;
    c["index"] = 0;
    c["delta"] = delta;
    chunk["choices"] = nlohmann::json::array({c});
    sse += "data: " + chunk.dump() + "\n\n";
  }

  // Usage chunk (stream_options.include_usage).
  nlohmann::json usage_chunk;
  usage_chunk["id"] = "mock";
  usage_chunk["object"] = "chat.completion.chunk";
  usage_chunk["choices"] = nlohmann::json::array();
  nlohmann::json usage;
  usage["prompt_tokens"] = 5;
  usage["completion_tokens"] = static_cast<int>(content.size());
  usage_chunk["usage"] = usage;
  sse += "data: " + usage_chunk.dump() + "\n\n";

  // Done marker.
  sse += "data: [DONE]\n\n";
  return sse;
}

bool WaitFor(std::function<bool()> pred, int timeout_ms = 8000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

// ---------------------------------------------------------------------------
// Test 1: Full Pipeline — StartSession → SendMessage → OnOutput → Shutdown
// ---------------------------------------------------------------------------
TEST(AgentRuntimeIntegrationTest, FullPipeline) {
  MockLlmServer mock([](const httplib::Request&, httplib::Response& res) {
    res.set_content(MakeChatResponse("hello from agent"), "application/json");
  });

  services::ToolRegistry tools;
  AgentRuntime runtime(MakeConfig(mock.BaseUrl()), tools);

  std::mutex mu;
  std::string received_text;
  runtime.OnOutput([&](const RuntimeOutput& out) {
    if (!out.is_partial) {
      std::lock_guard<std::mutex> lock(mu);
      received_text = out.text;
    }
  });

  std::string session_id = runtime.StartSession();
  ASSERT_FALSE(session_id.empty());

  runtime.SendMessage("hello");

  bool got_output = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !received_text.empty();
  });

  ASSERT_TRUE(got_output) << "OnOutput callback never fired with final response";
  {
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(received_text, "hello from agent");
  }

  runtime.Shutdown();
  EXPECT_EQ(runtime.GetState(), core::State::kTerminated);
}

// ---------------------------------------------------------------------------
// Test 2: Interrupt Path — SendMessage → slow streaming LLM → VAD speech_start → cancel
// ---------------------------------------------------------------------------
TEST(AgentRuntimeIntegrationTest, InterruptPath) {
  // Track whether the LLM handler was entered (to know when to interrupt).
  std::atomic<int> handler_entered{0};

  // The mock server streams SSE data very slowly on the first call.
  // Subsequent calls (after interrupt re-enqueues the observation) respond fast.
  MockLlmServer mock([&](const httplib::Request&, httplib::Response& res) {
    int n = handler_entered.fetch_add(1);
    if (n == 0) {
      // First call: stream slowly — interrupt should fire during the sleep.
      nlohmann::json chunk1;
      chunk1["id"] = "mock";
      chunk1["object"] = "chat.completion.chunk";
      nlohmann::json delta1;
      delta1["role"] = "assistant";
      delta1["content"] = "";
      nlohmann::json c1;
      c1["index"] = 0;
      c1["delta"] = delta1;
      chunk1["choices"] = nlohmann::json::array({c1});
      std::string first_chunk = "data: " + chunk1.dump() + "\n\n";

      nlohmann::json chunk2;
      chunk2["id"] = "mock";
      chunk2["object"] = "chat.completion.chunk";
      nlohmann::json delta2;
      delta2["content"] = "should not arrive";
      nlohmann::json c2;
      c2["index"] = 0;
      c2["delta"] = delta2;
      chunk2["choices"] = nlohmann::json::array({c2});
      std::string second_chunk = "data: " + chunk2.dump() + "\n\ndata: [DONE]\n\n";

      auto first = std::make_shared<std::string>(std::move(first_chunk));
      auto second = std::make_shared<std::string>(std::move(second_chunk));
      auto phase = std::make_shared<int>(0);

      res.set_content_provider(
          "text/event-stream",
          [first, second, phase](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            if (*phase == 0) {
              sink.write(first->data(), first->size());
              *phase = 1;
              // Sleep to simulate slow streaming.
              std::this_thread::sleep_for(std::chrono::seconds(2));
              return true;
            } else if (*phase == 1) {
              sink.write(second->data(), second->size());
              sink.done();
              *phase = 2;
              return true;
            }
            return false;
          });
    } else {
      // Subsequent calls: respond immediately with streaming response.
      res.set_content(MakeStreamingResponse("post interrupt"), "text/event-stream");
    }
  });

  services::ToolRegistry tools;
  // Use streaming mode so CurlPostStreaming checks cancel_requested_ between chunks.
  auto cfg = MakeConfig(mock.BaseUrl(), /*streaming=*/true);
  AgentRuntime runtime(cfg, tools);

  // Register a VAD event device BEFORE StartSession so the route gets wired.
  auto vad_dev = std::make_unique<MockIoDevice>(
      "vad_event",
      std::vector<io::PortDescriptor>{
          {"vad_out", io::PortDirection::kOutput, "vad/event"}});
  MockIoDevice* vad_ptr = vad_dev.get();
  runtime.RegisterDevice(std::move(vad_dev));

  // Track output — we expect NO final response for the interrupted turn.
  std::mutex mu;
  std::vector<std::string> final_outputs;
  runtime.OnOutput([&](const RuntimeOutput& out) {
    if (!out.is_partial) {
      std::lock_guard<std::mutex> lock(mu);
      final_outputs.push_back(out.text);
    }
  });

  runtime.StartSession();
  runtime.SendMessage("hello");

  // Wait for the LLM handler to be entered (loop thread is now in streaming).
  bool entered = WaitFor([&] { return handler_entered.load() >= 1; }, 3000);
  ASSERT_TRUE(entered) << "LLM handler was never called";

  // Wait a bit for the first SSE chunk to be received, then send speech_start.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Emit speech_start VAD event to trigger interrupt.
  io::DataFrame vad_frame;
  vad_frame.type = "vad/event";
  std::string speech_start = "speech_start";
  vad_frame.payload = std::vector<uint8_t>(speech_start.begin(), speech_start.end());
  vad_frame.timestamp = std::chrono::steady_clock::now();
  vad_ptr->EmitOutput("vad_out", std::move(vad_frame));

  // Wait for controller to transition to kListening after interrupt.
  // The controller may briefly pass through kListening then re-enter kThinking
  // (because the interrupt observation is re-enqueued as a user message).
  // We wait for it to settle back to kListening after processing.
  bool reached_listening = WaitFor([&] {
    auto state = runtime.GetState();
    return state == core::State::kListening;
  }, 8000);

  EXPECT_TRUE(reached_listening) << "Controller did not reach kListening after interrupt";

  // The interrupted turn's response ("should not arrive") must NOT have been delivered.
  // A subsequent turn from the re-enqueued empty observation may produce output.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& out : final_outputs) {
      EXPECT_NE(out, "should not arrive")
          << "Response from interrupted turn was delivered";
    }
  }

  runtime.Shutdown();
}

// ---------------------------------------------------------------------------
// Test 3: Tool Call Round-Trip
// ---------------------------------------------------------------------------
TEST(AgentRuntimeIntegrationTest, ToolCallRoundTrip) {
  std::atomic<int> call_count{0};

  MockLlmServer mock([&](const httplib::Request&, httplib::Response& res) {
    int n = call_count.fetch_add(1);
    if (n == 0) {
      // First call: return a tool call for "noop".
      res.set_content(
          MakeToolCallResponse("noop", "call_0", R"({})"),
          "application/json");
    } else {
      // Second call (after tool result): return final response.
      res.set_content(
          MakeChatResponse("tool call done"),
          "application/json");
    }
  });

  services::ToolRegistry tools;
  // Register a "noop" tool that always succeeds.
  tools.Register("noop", [](const std::string& /*args*/) -> services::ToolResult {
    return {.success = true, .output = "noop executed", .error_message = ""};
  });

  // Configure policy to allow the "noop" tool call.
  auto cfg = MakeConfig(mock.BaseUrl());
  core::PolicyRule allow_noop;
  allow_noop.priority = 0;
  allow_noop.action_pattern = "noop";
  allow_noop.outcome = core::PolicyOutcome::kAllow;
  cfg.policy.initial_rules.push_back(allow_noop);

  AgentRuntime runtime(cfg, tools);

  std::mutex mu;
  std::string final_response;
  runtime.OnOutput([&](const RuntimeOutput& out) {
    if (!out.is_partial) {
      std::lock_guard<std::mutex> lock(mu);
      final_response = out.text;
    }
  });

  runtime.StartSession();
  runtime.SendMessage("use tool");

  // Wait for the final response after the tool call round-trip.
  bool got_response = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !final_response.empty();
  }, 10000);

  ASSERT_TRUE(got_response)
      << "Final response never arrived after tool call round-trip";
  {
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(final_response, "tool call done");
  }

  // The LLM should have been called at least twice (tool call + final response).
  EXPECT_GE(call_count.load(), 2);

  runtime.Shutdown();
  EXPECT_EQ(runtime.GetState(), core::State::kTerminated);
}

}  // namespace
}  // namespace shizuru::runtime
