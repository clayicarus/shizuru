// Unit tests for services::json_parser (SerializeRequest, ParseResponse, ParseStreamChunk)

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "context/types.h"
#include "controller/types.h"
#include "interfaces/llm_client.h"
#include "llm/config.h"
#include "llm/openai/json_parser.h"

namespace shizuru::services {
namespace {

// ---------------------------------------------------------------------------
// SerializeRequest
// ---------------------------------------------------------------------------

TEST(JsonParserTest, SerializeRequest_BasicMessages) {
  core::ContextWindow ctx;
  ctx.messages = {
      {"system", "You are helpful.", "", ""},
      {"user", "Hello", "", ""},
  };
  ctx.estimated_tokens = 10;

  OpenAiConfig config;
  config.model = "gpt-4o";
  config.temperature = 0.5;
  config.max_tokens = 1024;

  std::string body = SerializeRequest(ctx, config);
  auto j = nlohmann::json::parse(body);

  EXPECT_EQ(j["model"], "gpt-4o");
  EXPECT_DOUBLE_EQ(j["temperature"].get<double>(), 0.5);
  EXPECT_EQ(j["max_tokens"], 1024);
  EXPECT_FALSE(j["stream"].get<bool>());

  ASSERT_EQ(j["messages"].size(), 2u);
  EXPECT_EQ(j["messages"][0]["role"], "system");
  EXPECT_EQ(j["messages"][0]["content"], "You are helpful.");
  EXPECT_EQ(j["messages"][1]["role"], "user");
  EXPECT_EQ(j["messages"][1]["content"], "Hello");
}

TEST(JsonParserTest, SerializeRequest_WithToolCallId) {
  core::ContextWindow ctx;
  ctx.messages = {
      {"system", "sys", "", ""},
      {"tool", "result data", "call_123", "my_tool"},
  };

  OpenAiConfig config;
  std::string body = SerializeRequest(ctx, config);
  auto j = nlohmann::json::parse(body);

  ASSERT_EQ(j["messages"].size(), 2u);
  EXPECT_EQ(j["messages"][1]["role"], "tool");
  EXPECT_EQ(j["messages"][1]["tool_call_id"], "call_123");
  EXPECT_EQ(j["messages"][1]["name"], "my_tool");
}

TEST(JsonParserTest, SerializeRequest_AssistantToolCallArgumentsAreString) {
  core::ContextWindow ctx;
  core::ContextMessage system_msg;
  system_msg.role = "system";
  system_msg.content = "sys";

  core::ContextMessage assistant_msg;
  assistant_msg.role = "assistant";
  assistant_msg.tool_calls_json =
      R"([{"id":"call_1","type":"function","function":{"name":"get_weather","arguments":{"city":"Tokyo"}}}])";

  ctx.messages = {system_msg, assistant_msg};

  OpenAiConfig config;
  std::string body = SerializeRequest(ctx, config);
  auto j = nlohmann::json::parse(body);

  ASSERT_EQ(j["messages"].size(), 2u);
  EXPECT_TRUE(j["messages"][1]["content"].is_null());
  ASSERT_EQ(j["messages"][1]["tool_calls"].size(), 1u);
  EXPECT_TRUE(j["messages"][1]["tool_calls"][0]["function"]["arguments"].is_string());
  EXPECT_EQ(j["messages"][1]["tool_calls"][0]["function"]["arguments"],
            R"({"city":"Tokyo"})");
}

TEST(JsonParserTest, SerializeRequest_NoToolsOmitsField) {
  core::ContextWindow ctx;
  ctx.messages = {{"user", "hi", "", ""}};

  OpenAiConfig config;
  config.tools.clear();

  std::string body = SerializeRequest(ctx, config);
  auto j = nlohmann::json::parse(body);

  EXPECT_FALSE(j.contains("tools"));
}

// ---------------------------------------------------------------------------
// SerializeTools
// ---------------------------------------------------------------------------

TEST(JsonParserTest, SerializeTools_SingleTool) {
  ToolDefinition tool;
  tool.name = "get_weather";
  tool.description = "Get current weather";
  tool.parameters = {
      {"location", "string", "City name", true},
      {"unit", "string", "Temperature unit", false},
  };

  auto arr = SerializeTools({tool});

  ASSERT_EQ(arr.size(), 1u);
  EXPECT_EQ(arr[0]["type"], "function");
  EXPECT_EQ(arr[0]["function"]["name"], "get_weather");
  EXPECT_EQ(arr[0]["function"]["description"], "Get current weather");

  auto& params = arr[0]["function"]["parameters"];
  EXPECT_EQ(params["type"], "object");
  EXPECT_TRUE(params["properties"].contains("location"));
  EXPECT_TRUE(params["properties"].contains("unit"));

  auto& required = params["required"];
  ASSERT_EQ(required.size(), 1u);
  EXPECT_EQ(required[0], "location");
}

// ---------------------------------------------------------------------------
// ParseResponse
// ---------------------------------------------------------------------------

TEST(JsonParserTest, ParseResponse_TextResponse) {
  nlohmann::json resp = {
      {"choices",
       {{{"message", {{"role", "assistant"}, {"content", "Hello!"}}},
         {"finish_reason", "stop"}}}},
      {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}}},
  };

  auto result = ParseResponse(resp.dump());

  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
  EXPECT_EQ(result.candidate.response_text, "Hello!");
  EXPECT_EQ(result.prompt_tokens, 10);
  EXPECT_EQ(result.completion_tokens, 5);
}

TEST(JsonParserTest, ParseResponse_ToolCall) {
  nlohmann::json resp = {
      {"choices",
       {{{"message",
          {{"role", "assistant"},
           {"content", nullptr},
           {"tool_calls",
            {{{"id", "call_abc"},
              {"type", "function"},
              {"function",
               {{"name", "get_weather"},
                {"arguments", "{\"location\":\"Tokyo\"}"}}}}}}}},
         {"finish_reason", "tool_calls"}}}},
      {"usage", {{"prompt_tokens", 20}, {"completion_tokens", 15}}},
  };

  auto result = ParseResponse(resp.dump());

  EXPECT_EQ(result.candidate.type, core::ActionType::kToolCall);
  ASSERT_EQ(result.candidate.tool_calls.size(), 1u);
  EXPECT_EQ(result.candidate.tool_calls[0].name, "get_weather");
  EXPECT_EQ(result.candidate.tool_calls[0].arguments, "{\"location\":\"Tokyo\"}");
  EXPECT_EQ(result.candidate.tool_calls[0].id, "call_abc");
  EXPECT_EQ(result.prompt_tokens, 20);
  EXPECT_EQ(result.completion_tokens, 15);
}

TEST(JsonParserTest, ParseResponse_EmptyContentIsContinue) {
  nlohmann::json resp = {
      {"choices",
       {{{"message", {{"role", "assistant"}, {"content", ""}}},
         {"finish_reason", "stop"}}}},
      {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 0}}},
  };

  auto result = ParseResponse(resp.dump());
  EXPECT_EQ(result.candidate.type, core::ActionType::kContinue);
}

TEST(JsonParserTest, ParseResponse_NullContentIsContinue) {
  nlohmann::json resp = {
      {"choices",
       {{{"message", {{"role", "assistant"}, {"content", nullptr}}},
         {"finish_reason", "stop"}}}},
      {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 0}}},
  };

  auto result = ParseResponse(resp.dump());
  EXPECT_EQ(result.candidate.type, core::ActionType::kContinue);
}

TEST(JsonParserTest, ParseResponse_ApiError) {
  nlohmann::json resp = {
      {"error", {{"message", "Rate limit exceeded"}, {"type", "rate_limit"}}},
  };

  EXPECT_THROW(ParseResponse(resp.dump()), std::runtime_error);
}

TEST(JsonParserTest, ParseResponse_MalformedJson) {
  EXPECT_THROW(ParseResponse("{invalid json"), std::runtime_error);
}

TEST(JsonParserTest, ParseResponse_NoChoices) {
  nlohmann::json resp = {{"choices", nlohmann::json::array()}};
  EXPECT_THROW(ParseResponse(resp.dump()), std::runtime_error);
}

// ---------------------------------------------------------------------------
// ParseStreamChunk
// ---------------------------------------------------------------------------

TEST(JsonParserTest, ParseStreamChunk_ContentDelta) {
  nlohmann::json chunk = {
      {"choices",
       {{{"delta", {{"content", "Hello"}}}, {"index", 0}}}},
  };

  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  bool ok = ParseStreamChunk("data: " + chunk.dump(), content, tool_calls,
                              result, is_done);

  EXPECT_TRUE(ok);
  EXPECT_FALSE(is_done);
  EXPECT_EQ(content, "Hello");
}

TEST(JsonParserTest, ParseStreamChunk_ToolCallDelta) {
  nlohmann::json tc;
  tc["index"] = 0;
  tc["id"] = "call_1";
  tc["type"] = "function";
  tc["function"] = {{"name", "search"}, {"arguments", "{\"q"}};

  nlohmann::json chunk;
  chunk["choices"] = nlohmann::json::array();
  nlohmann::json choice;
  choice["delta"]["tool_calls"] = nlohmann::json::array({tc});
  chunk["choices"].push_back(choice);

  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  bool ok = ParseStreamChunk("data: " + chunk.dump(), content, tool_calls,
                              result, is_done);

  EXPECT_TRUE(ok);
  EXPECT_FALSE(is_done);
  ASSERT_EQ(tool_calls.size(), 1u);
  EXPECT_EQ(tool_calls[0]["id"], "call_1");
  EXPECT_EQ(tool_calls[0]["function"]["name"], "search");
  EXPECT_EQ(tool_calls[0]["function"]["arguments"], "{\"q");
}

TEST(JsonParserTest, ParseStreamChunk_DoneMarker) {
  std::string content = "Hello world";
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  bool ok = ParseStreamChunk("data: [DONE]", content, tool_calls, result,
                              is_done);

  EXPECT_TRUE(ok);
  EXPECT_TRUE(is_done);
  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
  EXPECT_EQ(result.candidate.response_text, "Hello world");
}

TEST(JsonParserTest, ParseStreamChunk_DoneWithToolCalls) {
  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  tool_calls.push_back({
      {"id", "call_1"},
      {"function", {{"name", "search"}, {"arguments", "{\"q\":\"test\"}"}}},
  });
  core::LlmResult result;
  bool is_done = false;

  bool ok = ParseStreamChunk("data: [DONE]", content, tool_calls, result,
                              is_done);

  EXPECT_TRUE(ok);
  EXPECT_TRUE(is_done);
  EXPECT_EQ(result.candidate.type, core::ActionType::kToolCall);
  ASSERT_EQ(result.candidate.tool_calls.size(), 1u);
  EXPECT_EQ(result.candidate.tool_calls[0].name, "search");
  EXPECT_EQ(result.candidate.tool_calls[0].arguments, "{\"q\":\"test\"}");
  EXPECT_EQ(result.candidate.tool_calls[0].id, "call_1");
}

TEST(JsonParserTest, ParseStreamChunk_EmptyLine) {
  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  bool ok = ParseStreamChunk("", content, tool_calls, result, is_done);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(is_done);
}

// ---------------------------------------------------------------------------
// GenerateToolCallId
// ---------------------------------------------------------------------------

TEST(JsonParserTest, GenerateToolCallId_HasCorrectPrefix) {
  auto id = GenerateToolCallId();
  EXPECT_EQ(id.substr(0, 5), "call_");
  EXPECT_EQ(id.size(), 17u);  // "call_" (5) + 12 random chars
}

TEST(JsonParserTest, GenerateToolCallId_UniqueAcrossCalls) {
  std::set<std::string> ids;
  for (int i = 0; i < 100; ++i) {
    ids.insert(GenerateToolCallId());
  }
  EXPECT_EQ(ids.size(), 100u) << "Generated IDs should be unique";
}

TEST(JsonParserTest, ParseResponse_ToolCallMissingId_GeneratesOne) {
  nlohmann::json resp = {
      {"choices",
       {{{"message",
          {{"role", "assistant"},
           {"content", nullptr},
           {"tool_calls",
            {{{"type", "function"},
              {"function",
               {{"name", "my_tool"},
                {"arguments", "{}"}}}}}}}},
         {"finish_reason", "tool_calls"}}}},
      {"usage", {{"prompt_tokens", 5}, {"completion_tokens", 5}}},
  };

  auto result = ParseResponse(resp.dump());
  EXPECT_EQ(result.candidate.type, core::ActionType::kToolCall);
  ASSERT_EQ(result.candidate.tool_calls.size(), 1u);
  EXPECT_EQ(result.candidate.tool_calls[0].name, "my_tool");
  // ID should have been auto-generated with "call_" prefix.
  EXPECT_EQ(result.candidate.tool_calls[0].id.substr(0, 5), "call_");
  EXPECT_FALSE(result.candidate.tool_calls[0].id.empty());
}

// ---------------------------------------------------------------------------
// ParseStreamChunk — reasoning_content streaming
// ---------------------------------------------------------------------------

TEST(JsonParserTest, ParseStreamChunk_ReasoningContentWrappedInThinkTags) {
  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  // Send two reasoning_content chunks.
  nlohmann::json rc1 = {
      {"choices", {{{"delta", {{"reasoning_content", "Hello "}}}, {"index", 0}}}},
  };
  EXPECT_TRUE(ParseStreamChunk("data: " + rc1.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  nlohmann::json rc2 = {
      {"choices", {{{"delta", {{"reasoning_content", "world"}}}, {"index", 0}}}},
  };
  EXPECT_TRUE(ParseStreamChunk("data: " + rc2.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  // Send a content chunk.
  nlohmann::json cc = {
      {"choices", {{{"delta", {{"content", "response text"}}}, {"index", 0}}}},
  };
  EXPECT_TRUE(ParseStreamChunk("data: " + cc.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  // Verify accumulated_content before [DONE].
  EXPECT_EQ(content, "<think>Hello world</think>response text");

  // Send [DONE].
  EXPECT_TRUE(ParseStreamChunk("data: [DONE]", content, tool_calls, result,
                                is_done));
  EXPECT_TRUE(is_done);
  EXPECT_EQ(content, "<think>Hello world</think>response text");
  EXPECT_EQ(result.candidate.type, core::ActionType::kResponse);
  EXPECT_EQ(result.candidate.response_text,
            "<think>Hello world</think>response text");
}

TEST(JsonParserTest, ParseStreamChunk_ToolCallWithReasoningContent) {
  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  // Send reasoning_content chunk.
  nlohmann::json rc = {
      {"choices",
       {{{"delta", {{"reasoning_content", "I need to call a tool"}}},
         {"index", 0}}}},
  };
  EXPECT_TRUE(ParseStreamChunk("data: " + rc.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  // Send tool_calls delta chunks (name).
  nlohmann::json tc1;
  tc1["index"] = 0;
  tc1["id"] = "call_1";
  tc1["type"] = "function";
  tc1["function"] = {{"name", "get_time"}, {"arguments", ""}};
  nlohmann::json chunk1;
  chunk1["choices"] = nlohmann::json::array();
  nlohmann::json choice1;
  choice1["delta"]["tool_calls"] = nlohmann::json::array({tc1});
  chunk1["choices"].push_back(choice1);
  EXPECT_TRUE(ParseStreamChunk("data: " + chunk1.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  // Send tool_calls delta (arguments).
  nlohmann::json tc2;
  tc2["index"] = 0;
  tc2["function"] = {{"arguments", "{}"}};
  nlohmann::json chunk2;
  chunk2["choices"] = nlohmann::json::array();
  nlohmann::json choice2;
  choice2["delta"]["tool_calls"] = nlohmann::json::array({tc2});
  chunk2["choices"].push_back(choice2);
  EXPECT_TRUE(ParseStreamChunk("data: " + chunk2.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  // Send [DONE].
  EXPECT_TRUE(ParseStreamChunk("data: [DONE]", content, tool_calls, result,
                                is_done));
  EXPECT_TRUE(is_done);

  // Verify accumulated_content contains both thinking and tool_call markers.
  EXPECT_NE(content.find("<think>I need to call a tool</think>"),
            std::string::npos);
  EXPECT_NE(content.find("<tool_call>"), std::string::npos);
  EXPECT_NE(content.find("\"name\":\"get_time\""), std::string::npos);
  EXPECT_NE(content.find("\"id\":\"call_1\""), std::string::npos);

  // Verify result type and tool call details.
  EXPECT_EQ(result.candidate.type, core::ActionType::kToolCall);
  ASSERT_EQ(result.candidate.tool_calls.size(), 1u);
  EXPECT_EQ(result.candidate.tool_calls[0].name, "get_time");
}

TEST(JsonParserTest, ParseStreamChunk_ReasoningContentClosedAtDone) {
  std::string content;
  nlohmann::json tool_calls = nlohmann::json::array();
  core::LlmResult result;
  bool is_done = false;

  // Send reasoning_content chunk only.
  nlohmann::json rc = {
      {"choices",
       {{{"delta", {{"reasoning_content", "thinking..."}}}, {"index", 0}}}},
  };
  EXPECT_TRUE(ParseStreamChunk("data: " + rc.dump(), content, tool_calls,
                                result, is_done));
  EXPECT_FALSE(is_done);

  // Send [DONE] without any content chunks.
  EXPECT_TRUE(ParseStreamChunk("data: [DONE]", content, tool_calls, result,
                                is_done));
  EXPECT_TRUE(is_done);

  // The <think> tag should be closed at [DONE].
  EXPECT_EQ(content, "<think>thinking...</think>");
}

}  // namespace
}  // namespace shizuru::services
