// Unit and property-based tests for ToolDispatchDevice
// Uses Google Test + RapidCheck

// Feature: core-decoupling, Property 4: ToolDispatchDevice tool call round-trip

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "io/data_frame.h"
#include "runtime/tool_dispatch_device.h"
#include "runtime/tool_registry.h"

namespace shizuru::runtime {
namespace {

using io::DataFrame;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build an action/tool_call DataFrame with structured JSON payload.
DataFrame MakeToolCallFrame(const std::string& name,
                            const std::string& args) {
  nlohmann::json payload_json = {
      {"tool_name", name},
      {"arguments", args},
  };
  const std::string payload = payload_json.dump();
  DataFrame frame;
  frame.type = "action/tool_call";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());
  frame.timestamp = std::chrono::steady_clock::now();
  return frame;
}

// Wait up to timeout_ms for predicate to become true, polling every 5ms.
bool WaitFor(std::function<bool()> predicate, int timeout_ms = 500) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) { return true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

// ---------------------------------------------------------------------------
// Property 4: ToolDispatchDevice tool call round-trip
// **Validates: Requirements 3.3**
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ToolDispatchDevicePropTest, prop_tool_call_round_trip, ()) {
  const std::string name = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(
          rc::gen::inRange('a', static_cast<char>('z' + 1))));
  const std::string args = *rc::gen::container<std::string>(
      rc::gen::inRange('a', static_cast<char>('z' + 1)));
  const std::string return_value = *rc::gen::container<std::string>(
      rc::gen::inRange('a', static_cast<char>('z' + 1)));

  ToolRegistry registry;
  registry.Register(name, [return_value](const nlohmann::json&) {
    ToolResult r;
    r.success = true;
    r.output = return_value;
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  device.OnInput("action_in", MakeToolCallFrame(name, args));

  bool got_result = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  RC_ASSERT(got_result);

  std::lock_guard<std::mutex> lock(mu);
  RC_ASSERT(!emitted.empty());
  const auto& f = emitted.front();
  RC_ASSERT(f.type == "action/tool_result");

  const std::string payload(f.payload.begin(), f.payload.end());
  RC_ASSERT(payload.find(R"("success":true)") != std::string::npos);
  if (!return_value.empty()) {
    RC_ASSERT(payload.find(return_value) != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// Unit Test: Successful dispatch
// ---------------------------------------------------------------------------
TEST(ToolDispatchDeviceTest, SuccessfulDispatch) {
  ToolRegistry registry;
  registry.Register("echo", [](const nlohmann::json& args) {
    ToolResult r;
    r.success = true;
    r.output = args.is_string() ? args.get<std::string>() : args.dump();
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  device.OnInput("action_in", MakeToolCallFrame("echo", "hello"));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  const std::string payload(emitted[0].payload.begin(), emitted[0].payload.end());
  EXPECT_EQ(emitted[0].type, "action/tool_result");
  EXPECT_NE(payload.find(R"("success":true)"), std::string::npos);
  EXPECT_NE(payload.find("hello"), std::string::npos);
}

TEST(ToolDispatchDeviceTest, RoundTripPreservesToolCallId) {
  ToolRegistry registry;
  registry.Register("echo", [](const nlohmann::json& args) {
    ToolResult r;
    r.success = true;
    r.output = args;
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  auto frame = MakeToolCallFrame("echo", "{}");
  frame.metadata["tool_call_id"] = "call_test_1";
  device.OnInput("action_in", std::move(frame));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  const std::string payload(emitted[0].payload.begin(), emitted[0].payload.end());
  const auto json = nlohmann::json::parse(payload);
  EXPECT_EQ(json.value("tool_call_id", ""), "call_test_1");
}

// ---------------------------------------------------------------------------
// Unit Test: Unknown tool name → failure result frame
// ---------------------------------------------------------------------------
TEST(ToolDispatchDeviceTest, UnknownToolEmitsFailureFrame) {
  ToolRegistry registry;

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  device.OnInput("action_in", MakeToolCallFrame("no_such_tool", "{}"));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  const std::string payload(emitted[0].payload.begin(), emitted[0].payload.end());
  EXPECT_EQ(emitted[0].type, "action/tool_result");
  EXPECT_NE(payload.find(R"("success":false)"), std::string::npos);
  EXPECT_NE(payload.find("Unknown tool: no_such_tool"), std::string::npos);
}

TEST(ToolDispatchDeviceTest, MalformedPayloadEmitsFailureFrame) {
  ToolRegistry registry;
  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  DataFrame frame;
  frame.type = "action/tool_call";
  const std::string payload = "{not json";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());
  frame.timestamp = std::chrono::steady_clock::now();
  device.OnInput("action_in", std::move(frame));

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  const std::string result(emitted[0].payload.begin(), emitted[0].payload.end());
  EXPECT_NE(result.find(R"("success":false)"), std::string::npos);
  EXPECT_NE(result.find("Malformed tool call payload"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Unit Test: Tool throws exception → failure frame, device continues
// ---------------------------------------------------------------------------
TEST(ToolDispatchDeviceTest, ThrowingToolEmitsFailureAndDeviceContinues) {
  ToolRegistry registry;
  registry.Register("boom", [](const nlohmann::json&) -> ToolResult {
    throw std::runtime_error("intentional error");
  });
  registry.Register("ok", [](const nlohmann::json&) {
    ToolResult r;
    r.success = true;
    r.output = "survived";
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();
  device.OnInput("action_in", MakeToolCallFrame("boom", "{}"));
  device.OnInput("action_in", MakeToolCallFrame("ok", "{}"));

  bool got_two = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return emitted.size() >= 2;
  });

  device.Stop();

  ASSERT_TRUE(got_two) << "Expected 2 result frames, got "
                       << emitted.size();

  std::lock_guard<std::mutex> lock(mu);
  const std::string p0(emitted[0].payload.begin(), emitted[0].payload.end());
  EXPECT_NE(p0.find(R"("success":false)"), std::string::npos);
  EXPECT_NE(p0.find("intentional error"), std::string::npos);

  const std::string p1(emitted[1].payload.begin(), emitted[1].payload.end());
  EXPECT_NE(p1.find(R"("success":true)"), std::string::npos);
  EXPECT_NE(p1.find("survived"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Unit Test: Stop() drains queue before joining
// ---------------------------------------------------------------------------
TEST(ToolDispatchDeviceTest, StopDrainsQueueBeforeJoining) {
  ToolRegistry registry;

  std::atomic<int> executed{0};
  registry.Register("slow", [&](const nlohmann::json&) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ++executed;
    ToolResult r;
    r.success = true;
    r.output = "done";
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<DataFrame> emitted;
  device.SetOutputCallback([&](const std::string&, const std::string&,
                               DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device.Start();

  constexpr int kCount = 3;
  for (int i = 0; i < kCount; ++i) {
    device.OnInput("action_in", MakeToolCallFrame("slow", "{}"));
  }

  device.Stop();

  EXPECT_EQ(executed.load(), kCount);
  std::lock_guard<std::mutex> lock(mu);
  EXPECT_EQ(static_cast<int>(emitted.size()), kCount);
}

}  // namespace
}  // namespace shizuru::runtime
