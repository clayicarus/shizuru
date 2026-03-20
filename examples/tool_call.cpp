// Example: Agent runtime with tool calling (interactive mode).
//
// Demonstrates the full agent loop with interactive stdin/stdout:
//   User message -> LLM -> tool call -> tool result -> LLM -> final response
//
// Usage:
//   ./tool_call_example                          # mock LLM (no network needed)
//   ./tool_call_example <base_url> <api_key> [model] [api_path]
//
// Examples:
//   ./tool_call_example http://localhost:11434 "" qwen3:8b
//   ./tool_call_example https://api.openai.com sk-xxx gpt-4o
//
// Interactive commands:
//   Type any message and press Enter to send.
//   Type "quit" or "exit" or press Ctrl+D to end the session.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "agent_runtime.h"
#include "io/tool_registry.h"
#include "llm/config.h"
#include "policy/config.h"
#include "policy/types.h"
#include "spdlog/common.h"

namespace {

// --- Mock LLM server ---------------------------------------------------
// Simulates an OpenAI-compatible endpoint.
// Even-numbered calls return a tool_call to "get_weather".
// Odd-numbered calls return a plain text response.

class MockLlmServer {
 public:
  MockLlmServer() : port_(0), call_count_(0) {
    server_.Post("/v1/chat/completions",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   HandleRequest(req, res);
                 });
    port_ = server_.bind_to_any_port("127.0.0.1");
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    // Wait until server is ready.
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
  void HandleRequest(const httplib::Request& req, httplib::Response& res) {
    int call = call_count_++;

    // Extract the last user message to pick a city for the mock.
    std::string user_msg;
    try {
      auto body = nlohmann::json::parse(req.body);
      if (body.contains("messages") && body["messages"].is_array()) {
        for (auto it = body["messages"].rbegin();
             it != body["messages"].rend(); ++it) {
          if ((*it).value("role", "") == "user") {
            user_msg = (*it).value("content", "");
            break;
          }
        }
      }
    } catch (...) {}

    nlohmann::json resp;
    if (call % 2 == 0) {
      // Pick city from user message, default to Tokyo.
      std::string city = "Tokyo";
      if (user_msg.find("Beijing") != std::string::npos ||
          user_msg.find("\xe5\x8c\x97\xe4\xba\xac") != std::string::npos) {
        city = "Beijing";
      } else if (user_msg.find("London") != std::string::npos) {
        city = "London";
      } else if (user_msg.find("New York") != std::string::npos) {
        city = "New York";
      } else if (user_msg.find("Shanghai") != std::string::npos ||
                 user_msg.find("\xe4\xb8\x8a\xe6\xb5\xb7") != std::string::npos) {
        city = "Shanghai";
      }

      // Build tool_call response manually to avoid nested-brace ambiguity.
      nlohmann::json fn;
      fn["name"] = "get_weather";
      fn["arguments"] = R"({"city":")" + city + R"("})";

      nlohmann::json tc;
      tc["id"] = "call_" + std::to_string(call);
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
      usage["prompt_tokens"] = 30;
      usage["completion_tokens"] = 20;

      resp["id"] = "chatcmpl-mock-" + std::to_string(call);
      resp["object"] = "chat.completion";
      resp["choices"] = nlohmann::json::array({choice});
      resp["usage"] = usage;
    } else {
      nlohmann::json msg;
      msg["role"] = "assistant";
      msg["content"] = "I've checked the weather for you. "
                       "It's 22 degrees and sunny. Anything else?";

      nlohmann::json choice;
      choice["index"] = 0;
      choice["message"] = msg;
      choice["finish_reason"] = "stop";

      nlohmann::json usage;
      usage["prompt_tokens"] = 50;
      usage["completion_tokens"] = 15;

      resp["id"] = "chatcmpl-mock-" + std::to_string(call);
      resp["object"] = "chat.completion";
      resp["choices"] = nlohmann::json::array({choice});
      resp["usage"] = usage;
    }

    res.set_content(resp.dump(), "application/json");
  }

  httplib::Server server_;
  int port_;
  std::thread thread_;
  std::atomic<int> call_count_;
};

}  // namespace

int main(int argc, char* argv[]) {
  std::printf("=== Shizuru Tool Call Example (Interactive) ===\n");
  std::printf("Type a message and press Enter. Type 'quit' or Ctrl+D to exit.\n\n");

  // Parse optional CLI args: <base_url> <api_key> [model] [api_path]
  std::string base_url;
  std::string api_key;
  std::string model;
  std::string api_path;

  if (argc >= 3) {
    base_url = argv[1];
    api_key  = argv[2];
    model    = (argc >= 4) ? argv[3] : "gpt-4o";
    api_path = (argc >= 5) ? argv[4] : "";
  }

  // If no URL provided, start mock server as fallback.
  std::unique_ptr<MockLlmServer> mock;
  if (base_url.empty()) {
    mock = std::make_unique<MockLlmServer>();
    base_url = mock->BaseUrl();
    api_key  = "mock-key";
    model    = "mock-model";
    std::printf("[setup] No LLM URL provided, using mock server at %s\n\n",
                base_url.c_str());
  } else {
    std::printf("[setup] Using LLM at %s (model: %s)\n\n",
                base_url.c_str(), model.c_str());
  }

  // Register tools.
  shizuru::services::ToolRegistry tools;
  tools.Register("get_weather",
                 [](const std::string& args) -> shizuru::core::ActionResult {
                   auto j = nlohmann::json::parse(args);
                   std::string city = j.value("city", "unknown");
                   std::printf("[tool]  get_weather(city=%s)\n", city.c_str());
                   nlohmann::json result;
                   result["city"]        = city;
                   result["temperature"] = "22 degrees";
                   result["condition"]   = "sunny";
                   return {true, result.dump(), ""};
                 });

  // Configure the runtime.
  shizuru::runtime::RuntimeConfig config;
  config.logger.level              = spdlog::level::debug;
  config.llm.base_url              = base_url;
  config.llm.api_key               = api_key;
  config.llm.model                 = model;
  config.llm.connect_timeout       = std::chrono::seconds(5);
  config.llm.read_timeout          = std::chrono::seconds(30);
  if (!api_path.empty()) {
    config.llm.api_path = api_path;
  }

  shizuru::services::ToolDefinition weather_tool;
  weather_tool.name        = "get_weather";
  weather_tool.description = "Get current weather for a city";
  weather_tool.parameters  = {{"city", "string", "City name", true}};
  config.llm.tools         = {weather_tool};

  config.policy.default_capabilities = {"get_weather"};
  {
    shizuru::core::PolicyRule allow_weather;
    allow_weather.priority = 0;
    allow_weather.action_pattern = "get_weather";
    allow_weather.required_capability = "get_weather";
    allow_weather.outcome = shizuru::core::PolicyOutcome::kAllow;
    config.policy.initial_rules = {allow_weather};
  }
  config.controller.max_turns        = 100;

  // Create runtime.
  shizuru::runtime::AgentRuntime runtime(config, tools);

  // Synchronization: block the input loop until the agent replies.
  std::mutex            resp_mutex;
  std::condition_variable resp_cv;
  std::atomic<bool>     waiting{false};

  runtime.OnOutput([&](const shizuru::runtime::RuntimeOutput& output) {
    std::printf("\n[agent] %s\n", output.text.c_str());
    std::fflush(stdout);
    {
      std::lock_guard<std::mutex> lk(resp_mutex);
      waiting.store(false);
    }
    resp_cv.notify_one();
  });

  // Start session.
  std::string session_id = runtime.StartSession();
  std::printf("[session] %s started\n\n", session_id.c_str());

  // Interactive loop.
  std::string line;
  while (true) {
    std::printf("You> ");
    std::fflush(stdout);

    if (!std::getline(std::cin, line)) {
      std::printf("\n");
      break;  // EOF / Ctrl+D
    }

    // Trim whitespace.
    const auto s = line.find_first_not_of(" \t\r\n");
    const auto e = line.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) { continue; }
    line = line.substr(s, e - s + 1);

    if (line == "quit" || line == "exit") { break; }

    // Send and wait for response.
    waiting.store(true);
    runtime.SendMessage(line);

    {
      std::unique_lock<std::mutex> lk(resp_mutex);
      resp_cv.wait_for(lk, std::chrono::seconds(60),
                       [&] { return !waiting.load(); });
    }

    if (waiting.load()) {
      std::printf("\n[timeout] No response within 60 seconds.\n");
      break;
    }
  }

  runtime.Shutdown();
  std::printf("\n[session] ended.\n");
  return 0;
}
