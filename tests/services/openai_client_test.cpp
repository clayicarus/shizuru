// Unit tests for services::OpenAiClient
// Uses a local httplib::Server to mock the OpenAI API.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "context/types.h"
#include "controller/types.h"
#include "interfaces/llm_client.h"
#include "llm/config.h"
#include "llm/openai_client.h"
#include "test_utils.h"

namespace shizuru::services {
namespace {

// RAII wrapper that starts an httplib::Server on a background thread
// and stops it on destruction. Waits until the server is actually accepting
// connections before returning from the constructor.
class MockServer {
 public:
  explicit MockServer(httplib::Server& server) : server_(server) {
    port_ = server_.bind_to_any_port("127.0.0.1");
    EXPECT_GT(port_, 0);
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    WaitUntilReady();
  }

  ~MockServer() {
    server_.stop();
    if (thread_.joinable()) thread_.join();
  }

  int Port() const { return port_; }
  std::string BaseUrl() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  void WaitUntilReady() {
    // Poll until the server accepts a TCP connection.
    for (int i = 0; i < 100; ++i) {
      httplib::Client cli(BaseUrl());
      cli.set_connection_timeout(std::chrono::milliseconds(50));
      // A simple GET to a non-existent path — we just need the connection
      // to succeed (even a 404 means the server is up).
      auto res = cli.Get("/healthz");
      if (res) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL() << "MockServer did not become ready within timeout";
  }

  httplib::Server& server_;
  int port_ = 0;
  std::thread thread_;
};

// Helper: build a minimal ContextWindow with one user message.
core::ContextWindow MakeSimpleContext(const std::string& user_msg = "Hello") {
  core::ContextWindow ctx;
  ctx.messages = {
      {"system", "You are helpful.", "", ""},
      {"user", user_msg, "", ""},
  };
  ctx.estimated_tokens = 10;
  return ctx;
}

// Helper: build an OpenAiConfig pointing at the mock server.
OpenAiConfig MakeConfig(const std::string& base_url) {
  OpenAiConfig cfg;
  cfg.api_key = "test-key";
  cfg.base_url = base_url;
  cfg.model = "test-model";
  cfg.temperature = 0.5;
  cfg.max_tokens = 100;
  cfg.connect_timeout = std::chrono::seconds(5);
  cfg.read_timeout = std::chrono::seconds(5);
  return cfg;
}

// Helper: build a standard text completion JSON response.
std::string MakeTextResponse(const std::string& content,
                             int prompt_tokens = 10,
                             int completion_tokens = 5) {
  nlohmann::json resp = {
      {"id", "chatcmpl-test"},
      {"object", "chat.completion"},
      {"choices",
       {{{"index", 0},
         {"message", {{"role", "assistant"}, {"content", content}}},
         {"finish_reason", "stop"}}}},
      {"usage",
       {{"prompt_tokens", prompt_tokens},
        {"completion_tokens", completion_tokens}}},
  };
  return resp.dump();
}

// Helper: build a tool call completion JSON response.
std::string MakeToolCallResponse(const std::string& tool_name,
                                 const std::string& arguments,
                                 const std::string& call_id = "call_123") {
  nlohmann::json resp = {
      {"id", "chatcmpl-test"},
      {"object", "chat.completion"},
      {"choices",
       {{{"index", 0},
         {"message",
          {{"role", "assistant"},
           {"content", nullptr},
           {"tool_calls",
            {{{"id", call_id},
              {"type", "function"},
              {"function",
               {{"name", tool_name}, {"arguments", arguments}}}}}}}},
         {"finish_reason", "tool_calls"}}}},
      {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 15}}},
  };
  return resp.dump();
}

// ---------------------------------------------------------------------------
// Submit (non-streaming)
// ---------------------------------------------------------------------------

TEST(OpenAiClientTest, Submit_TextResponse) {
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request& req, httplib::Response& res) {
             // Verify request has Authorization header.
             EXPECT_TRUE(req.has_header("Authorization"));
             EXPECT_EQ(req.get_header_value("Authorization"),
                       "Bearer test-key");

             // Verify request body is valid JSON with expected fields.
             auto body = nlohmann::json::parse(req.body);
             EXPECT_EQ(body["model"], "test-model");
             EXPECT_FALSE(body["stream"].get<bool>());
             EXPECT_EQ(body["messages"].size(), 2u);

             res.set_content(MakeTextResponse("Hi there!"),
                             "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  auto result = client.Submit(MakeSimpleContext());

  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
  EXPECT_EQ(result.candidate.response_text, "Hi there!");
  EXPECT_EQ(result.prompt_tokens, 10);
  EXPECT_EQ(result.completion_tokens, 5);
}

TEST(OpenAiClientTest, Submit_ToolCallResponse) {
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request&, httplib::Response& res) {
             res.set_content(
                 MakeToolCallResponse("get_weather", R"({"city":"Tokyo"})"),
                 "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  auto result = client.Submit(MakeSimpleContext());

  EXPECT_EQ(result.candidate.type, core::ActionType::kToolCall);
  EXPECT_EQ(result.candidate.action_name, "get_weather");
  EXPECT_EQ(result.candidate.arguments, R"({"city":"Tokyo"})");
  EXPECT_EQ(result.prompt_tokens, 20);
  EXPECT_EQ(result.completion_tokens, 15);
}

TEST(OpenAiClientTest, Submit_ServerReturns500) {
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 500;
             res.set_content(R"({"error":"internal"})", "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  EXPECT_THROW(client.Submit(MakeSimpleContext()), std::runtime_error);
}

TEST(OpenAiClientTest, Submit_ConnectionRefused) {
  // Point at a port where nothing is listening.
  auto config = MakeConfig("http://127.0.0.1:1");
  config.connect_timeout = std::chrono::seconds(1);
  OpenAiClient client(config);

  EXPECT_THROW(client.Submit(MakeSimpleContext()), std::runtime_error);
}

TEST(OpenAiClientTest, Submit_WithToolDefinitions) {
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request& req, httplib::Response& res) {
             auto body = nlohmann::json::parse(req.body);
             // Verify tools are serialized in the request.
             EXPECT_TRUE(body.contains("tools"));
             EXPECT_EQ(body["tools"].size(), 1u);
             EXPECT_EQ(body["tools"][0]["function"]["name"], "search");

             res.set_content(MakeTextResponse("done"), "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  config.tools = {{
      "search",
      "Search the web",
      {{"query", "string", "Search query", true}},
      "web_access",
  }};
  OpenAiClient client(config);

  auto result = client.Submit(MakeSimpleContext());
  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
}

TEST(OpenAiClientTest, Submit_CustomApiPath) {
  httplib::Server svr;
  svr.Post("/api/generate",
           [](const httplib::Request&, httplib::Response& res) {
             res.set_content(MakeTextResponse("custom path works"),
                             "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  config.api_path = "/api/generate";
  OpenAiClient client(config);

  auto result = client.Submit(MakeSimpleContext());
  EXPECT_EQ(result.candidate.response_text, "custom path works");
}

// ---------------------------------------------------------------------------
// SubmitStreaming
// ---------------------------------------------------------------------------

TEST(OpenAiClientTest, SubmitStreaming_TextResponse) {
  LOG("[SubmitStreaming_TextResponse] Setting up mock server");

  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request& req, httplib::Response& res) {
             auto body = nlohmann::json::parse(req.body);
             LOG("[SubmitStreaming_TextResponse] Received request, stream={}",
                          body["stream"].get<bool>());
             EXPECT_TRUE(body["stream"].get<bool>());

             // Build SSE response with multiple chunks.
             std::string sse;

             // Chunk 1: content delta "Hello"
             nlohmann::json c1 = {
                 {"choices", {{{"delta", {{"content", "Hello"}}},
                               {"index", 0}}}},
             };
             sse += "data: " + c1.dump() + "\n\n";
             LOG("[SubmitStreaming_TextResponse] Prepared chunk 1: {}", c1.dump());

             // Chunk 2: content delta " world"
             nlohmann::json c2 = {
                 {"choices", {{{"delta", {{"content", " world"}}},
                               {"index", 0}}}},
             };
             sse += "data: " + c2.dump() + "\n\n";
             LOG("[SubmitStreaming_TextResponse] Prepared chunk 2: {}", c2.dump());

             // Usage chunk (no choices).
             nlohmann::json usage_chunk = {
                 {"usage",
                  {{"prompt_tokens", 8}, {"completion_tokens", 3}}},
             };
             sse += "data: " + usage_chunk.dump() + "\n\n";
             LOG("[SubmitStreaming_TextResponse] Prepared usage chunk: {}", usage_chunk.dump());

             // Done marker.
             sse += "data: [DONE]\n\n";

             LOG("[SubmitStreaming_TextResponse] Sending SSE response ({} bytes)", sse.size());
             res.set_content(sse, "text/event-stream");
           });

  MockServer mock(svr);
  LOG("[SubmitStreaming_TextResponse] Mock server ready at {}", mock.BaseUrl());

  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  std::vector<std::string> tokens;
  LOG("[SubmitStreaming_TextResponse] Calling SubmitStreaming");
  auto result = client.SubmitStreaming(
      MakeSimpleContext(),
      [&tokens](const std::string& token) {
        LOG("[SubmitStreaming_TextResponse] Received token: \"{}\"", token);
        tokens.push_back(token);
      });

  LOG("[SubmitStreaming_TextResponse] Result: type={}, text=\"{}\", "
               "prompt_tokens={}, completion_tokens={}",
               static_cast<int>(result.candidate.type),
               result.candidate.response_text,
               result.prompt_tokens, result.completion_tokens);

  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
  EXPECT_EQ(result.candidate.response_text, "Hello world");
  EXPECT_EQ(result.prompt_tokens, 8);
  EXPECT_EQ(result.completion_tokens, 3);

  // Verify streaming callback received incremental tokens.
  ASSERT_GE(tokens.size(), 1u);
  std::string joined;
  for (const auto& t : tokens) joined += t;
  LOG("[SubmitStreaming_TextResponse] Joined tokens: \"{}\", count={}",
               joined, tokens.size());
  EXPECT_EQ(joined, "Hello world");
}

TEST(OpenAiClientTest, SubmitStreaming_ToolCallResponse) {
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request&, httplib::Response& res) {
             std::string sse;

             // First chunk: tool call start.
             nlohmann::json tc1;
             tc1["choices"] = nlohmann::json::array();
             nlohmann::json delta1;
             delta1["delta"]["tool_calls"] = nlohmann::json::array();
             delta1["delta"]["tool_calls"].push_back({
                 {"index", 0},
                 {"id", "call_abc"},
                 {"type", "function"},
                 {"function", {{"name", "get_weather"}, {"arguments", ""}}},
             });
             tc1["choices"].push_back(delta1);
             sse += "data: " + tc1.dump() + "\n\n";

             // Second chunk: arguments delta.
             nlohmann::json tc2;
             tc2["choices"] = nlohmann::json::array();
             nlohmann::json delta2;
             delta2["delta"]["tool_calls"] = nlohmann::json::array();
             delta2["delta"]["tool_calls"].push_back({
                 {"index", 0},
                 {"function", {{"arguments", R"({"city":"NYC"})"}}},
             });
             tc2["choices"].push_back(delta2);
             sse += "data: " + tc2.dump() + "\n\n";

             sse += "data: [DONE]\n\n";

             res.set_content(sse, "text/event-stream");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  auto result = client.SubmitStreaming(MakeSimpleContext(), nullptr);

  EXPECT_EQ(result.candidate.type, core::ActionType::kToolCall);
  EXPECT_EQ(result.candidate.action_name, "get_weather");
  EXPECT_EQ(result.candidate.arguments, R"({"city":"NYC"})");
}

TEST(OpenAiClientTest, SubmitStreaming_ServerError) {
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request&, httplib::Response& res) {
             res.status = 429;
             res.set_content(R"({"error":"rate limited"})",
                             "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  EXPECT_THROW(client.SubmitStreaming(MakeSimpleContext(), nullptr),
               std::runtime_error);
}

TEST(OpenAiClientTest, SubmitStreaming_NoDoneMarkerFallback) {
  // Server sends content chunks but no [DONE] marker.
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request&, httplib::Response& res) {
             std::string sse;
             nlohmann::json c1 = {
                 {"choices",
                  {{{"delta", {{"content", "partial"}}}, {"index", 0}}}},
             };
             sse += "data: " + c1.dump() + "\n\n";
             // No [DONE] — stream just ends.
             res.set_content(sse, "text/event-stream");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  auto result = client.SubmitStreaming(MakeSimpleContext(), nullptr);

  // Should fall back to building result from accumulated content.
  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
  EXPECT_EQ(result.candidate.response_text, "partial");
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST(OpenAiClientTest, Cancel_SetsFlag) {
  // Verify that Cancel() sets the internal flag. Since the actual abort
  // behavior depends on timing with a real HTTP connection, we just verify
  // the flag mechanism works and doesn't crash.
  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [](const httplib::Request&, httplib::Response& res) {
             res.set_content(MakeTextResponse("ok"), "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  OpenAiClient client(config);

  // Cancel before any request — should not crash.
  EXPECT_NO_THROW(client.Cancel());

  // Subsequent request should still work (cancel flag is reset on Submit).
  auto result = client.Submit(MakeSimpleContext());
  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
}

// ---------------------------------------------------------------------------
// Request serialization verification
// ---------------------------------------------------------------------------

TEST(OpenAiClientTest, Submit_RequestContainsCorrectFields) {
  nlohmann::json captured_body;

  httplib::Server svr;
  svr.Post("/v1/chat/completions",
           [&captured_body](const httplib::Request& req,
                            httplib::Response& res) {
             captured_body = nlohmann::json::parse(req.body);
             res.set_content(MakeTextResponse("ok"), "application/json");
           });

  MockServer mock(svr);
  auto config = MakeConfig(mock.BaseUrl());
  config.temperature = 0.9;
  config.max_tokens = 2048;
  OpenAiClient client(config);

  client.Submit(MakeSimpleContext("What is 2+2?"));

  EXPECT_EQ(captured_body["model"], "test-model");
  EXPECT_DOUBLE_EQ(captured_body["temperature"].get<double>(), 0.9);
  EXPECT_EQ(captured_body["max_tokens"], 2048);
  ASSERT_EQ(captured_body["messages"].size(), 2u);
  EXPECT_EQ(captured_body["messages"][0]["role"], "system");
  EXPECT_EQ(captured_body["messages"][1]["content"], "What is 2+2?");
}

}  // namespace
}  // namespace shizuru::services
