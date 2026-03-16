// Example: Agent runtime with tool calling.
//
// Demonstrates the full agent loop:
//   User message → LLM → tool call → tool result → LLM → final response
//
// Usage:
//   ./tool_call_example                          # mock LLM (no network needed)
//   ./tool_call_example <base_url> <api_key> [model]
//
// Examples:
//   ./tool_call_example http://localhost:11434 "" qwen3:8b
//   ./tool_call_example https://api.openai.com sk-xxx gpt-4o

#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "agent_runtime.h"
#include "io/tool_registry.h"
#include "llm/config.h"

namespace {

// --- Mock LLM server ---------------------------------------------------
// Simulates an OpenAI-compatible endpoint that:
//   1st call: returns a tool_call to "get_weather"
//   2nd call: returns a text response using the tool result

class MockLlmServer {
 public:
  MockLlmServer() {
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   HandleRequest(req, res);
                 });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    for (int i = 0; i < 100; ++i) {
      httplib::Client cli(BaseUrl());
      cli.set_connection_timeout(std::chrono::milliseconds(50));
      if (cli.Get("/healthz")) { break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  ~MockLlmServer() {
    server_.stop();
    if (thread_.joinable()) { thread_.join(); }
  }

  std::string BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  void HandleRequest(const httplib::Request& /*req*/, httplib::Response& res) {
    int call = call_count_++;
    nlohmann::json resp;

    if (call == 0) {
      resp = {
          {"id", "chatcmpl-mock-1"},
          {"object", "chat.completion"},
          {"choices",
           {{{"index", 0},
             {"message",
              {{"role", "assistant"},
               {"content", nullptr},
               {"tool_calls",
                {{{"id", "call_001"},
                  {"type", "function"},
                  {"function",
                   {{"name", "get_weather"},
                    {"arguments", R"({"city":"Tokyo"})"}}}}}}}},
             {"finish_reason", "tool_calls"}}}},
          {"usage", {{"prompt_tokens", 30}, {"completion_tokens", 20}}},
      };
    } else {
      resp = {
          {"id", "chatcmpl-mock-2"},
          {"object", "chat.completion"},
          {"choices",
           {{{"index", 0},
             {"message",
              {{"role", "assistant"},
               {"content", "The weather in Tokyo is 22°C and sunny."}}},
             {"finish_reason", "stop"}}}},
          {"usage", {{"prompt_tokens", 50}, {"completion_tokens", 15}}},
      };
    }

    res.set_content(resp.dump(), "application/json");
  }

  httplib::Server server_;
  int port_ = 0;
  std::thread thread_;
  std::atomic<int> call_count_{0};
};

}  // namespace

int main(int argc, char* argv[]) {
  std::printf("=== Shizuru Tool Call Example ===\n\n");

  // Parse optional CLI args: <base_url> <api_key> [model] [api_path]
  std::string base_url;
  std::string api_key;
  std::string model;
  std::string api_path;

  if (argc >= 3) {
    base_url = argv[1];
    api_key = argv[2];
    model = (argc >= 4) ? argv[3] : "gpt-4o";
    api_path = (argc >= 5) ? argv[4] : "";
  }

  // If no URL provided, start mock server as fallback.
  std::unique_ptr<MockLlmServer> mock;
  if (base_url.empty()) {
    mock = std::make_unique<MockLlmServer>();
    base_url = mock->BaseUrl();
    api_key = "mock-key";
    model = "mock-model";
    std::printf("[setup] No LLM URL provided, using mock server at %s\n",
                base_url.c_str());
  } else {
    std::printf("[setup] Using LLM at %s (model: %s)\n",
                base_url.c_str(), model.c_str());
  }

  // Register a "get_weather" tool.
  shizuru::services::ToolRegistry tools;
  tools.Register("get_weather",
                 [](const std::string& args) -> shizuru::core::ActionResult {
                   auto j = nlohmann::json::parse(args);
                   std::string city = j.value("city", "unknown");
                   std::printf("[tool]  get_weather called with city=%s\n",
                               city.c_str());

                   nlohmann::json result;
                   result["city"] = city;
                   result["temperature"] = "22°C";
                   result["condition"] = "sunny";
                   return {true, result.dump(), ""};
                 });

  // Configure the runtime.
  shizuru::runtime::RuntimeConfig config;
  config.llm.base_url = base_url;
  config.llm.api_key = api_key;
  config.llm.model = model;
  if (!api_path.empty()) {
    config.llm.api_path = api_path;
  }
  config.llm.connect_timeout = std::chrono::seconds(5);
  config.llm.read_timeout = std::chrono::seconds(30);

  config.llm.tools = {{
      "get_weather",
      "Get current weather for a city",
      {{"city", "string", "City name", true}},
      "",
  }};

  config.policy.default_capabilities = {"get_weather"};
  config.controller.max_turns = 10;
  config.auto_tts_on_audio_input = false;

  // Create runtime and register output callback.
  shizuru::runtime::AgentRuntime runtime(config, tools);

  std::mutex done_mutex;
  std::condition_variable done_cv;
  bool got_response = false;

  runtime.OnOutput([&](const shizuru::runtime::RuntimeOutput& output) {
    std::printf("\n[agent] Response: %s\n", output.text.c_str());
    std::lock_guard<std::mutex> lock(done_mutex);
    got_response = true;
    done_cv.notify_one();
  });

  // Start session and send a message.
  std::string session_id = runtime.StartSession();
  std::printf("[runtime] Session started: %s\n", session_id.c_str());
  std::printf("[user]   \"What's the weather in Tokyo?\"\n");

  runtime.Send("What's the weather in Tokyo?");

  // Wait for the agent to produce a final response.
  {
    std::unique_lock<std::mutex> lock(done_mutex);
    done_cv.wait_for(lock, std::chrono::seconds(30),
                     [&] { return got_response; });
  }

  if (!got_response) {
    std::printf("\n[timeout] No response within 30 seconds.\n");
  }

  runtime.Shutdown();
  std::printf("\n[runtime] Session ended.\n");
  return 0;
}
