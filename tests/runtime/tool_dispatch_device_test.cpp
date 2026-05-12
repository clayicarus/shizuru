// Unit and property-based tests for ToolDispatchDevice
// Uses Google Test + RapidCheck

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

#include "core/control_signal.h"
#include "runtime/tool_dispatch_device.h"
#include "runtime/tool_registry.h"

namespace shizuru::runtime {
namespace {

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

core::ToolCallStartSignal MakeToolCallSignal(const std::string& id,
                                             const std::string& name,
                                             const std::string& args) {
  return core::ToolCallStartSignal{id, name, args};
}

void DeliverToolCall(ToolDispatchDevice& device,
                     const std::string& id,
                     const std::string& name,
                     const std::string& args) {
  device.OnControlSignal("control_in", MakeToolCallSignal(id, name, args));
}

// ---------------------------------------------------------------------------
// Property: typed tool call round-trip
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ToolDispatchDevicePropTest, prop_tool_call_round_trip, ()) {
  const std::string name = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(
          rc::gen::inRange('a', static_cast<char>('z' + 1))));
  const std::string return_value = *rc::gen::container<std::string>(
      rc::gen::inRange('a', static_cast<char>('z' + 1)));
  const std::string tool_call_id = *rc::gen::nonEmpty(
      rc::gen::container<std::string>(
          rc::gen::inRange('0', static_cast<char>('9' + 1))));

  ToolRegistry registry;
  registry.Register(name, [return_value](const nlohmann::json&) {
    ToolResult r;
    r.success = true;
    r.output = return_value;
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  DeliverToolCall(device, tool_call_id, name, "{}");

  bool got_result = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  RC_ASSERT(got_result);

  std::lock_guard<std::mutex> lock(mu);
  RC_ASSERT(emitted.size() == 1);
  RC_ASSERT(emitted[0].tool_call_id == tool_call_id);
  RC_ASSERT(emitted[0].success);
  if (!return_value.empty()) {
    RC_ASSERT(emitted[0].content.find(return_value) != std::string::npos);
  }
}

TEST(ToolDispatchDeviceTest, SuccessfulDispatch) {
  ToolRegistry registry;
  registry.Register("echo", [](const nlohmann::json& args) {
    ToolResult r;
    r.success = true;
    r.output = args;
    return r;
  });

  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  DeliverToolCall(device, "call_1", "echo", R"({"value":"hello"})");

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].tool_call_id, "call_1");
  EXPECT_TRUE(emitted[0].success);
  EXPECT_NE(emitted[0].content.find("hello"), std::string::npos);
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
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  DeliverToolCall(device, "call_test_1", "echo", "{}");

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].tool_call_id, "call_test_1");
}

TEST(ToolDispatchDeviceTest, UnknownToolEmitsFailureSignal) {
  ToolRegistry registry;
  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  DeliverToolCall(device, "call_unknown", "no_such_tool", "{}");

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_FALSE(emitted[0].success);
  EXPECT_NE(emitted[0].content.find("Unknown tool: no_such_tool"),
            std::string::npos);
}

TEST(ToolDispatchDeviceTest, MalformedArgumentsEmitFailureSignal) {
  ToolRegistry registry;
  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  DeliverToolCall(device, "call_bad_json", "echo", "{not json");

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  });

  device.Stop();

  ASSERT_TRUE(got);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_FALSE(emitted[0].success);
  EXPECT_NE(emitted[0].content.find("Malformed tool call arguments JSON"),
            std::string::npos);
}

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
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  DeliverToolCall(device, "call_boom", "boom", "{}");
  DeliverToolCall(device, "call_ok", "ok", "{}");

  bool got_two = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return emitted.size() >= 2;
  });

  device.Stop();

  ASSERT_TRUE(got_two);
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(emitted.size(), 2u);
  EXPECT_FALSE(emitted[0].success);
  EXPECT_NE(emitted[0].content.find("intentional error"), std::string::npos);
  EXPECT_TRUE(emitted[1].success);
  EXPECT_NE(emitted[1].content.find("survived"), std::string::npos);
}

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
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();

  constexpr int kCount = 3;
  for (int i = 0; i < kCount; ++i) {
    DeliverToolCall(device, "slow_" + std::to_string(i), "slow", "{}");
  }

  device.Stop();

  EXPECT_EQ(executed.load(), kCount);
  std::lock_guard<std::mutex> lock(mu);
  EXPECT_EQ(static_cast<int>(emitted.size()), kCount);
}

TEST(ToolDispatchDeviceTest, NonToolControlSignalsDoNotEmitResults) {
  ToolRegistry registry;
  ToolDispatchDevice device(registry);

  std::mutex mu;
  std::vector<core::ToolResultSignal> emitted;
  device.SetControlSignalOutputCallback(
      [&](const std::string&, const std::string&, core::ControlSignal sig) {
        if (auto* result = std::get_if<core::ToolResultSignal>(&sig)) {
          std::lock_guard<std::mutex> lock(mu);
          emitted.push_back(*result);
        }
      });

  device.Start();
  device.OnControlSignal("control_in", core::FlushSignal{});

  bool got = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !emitted.empty();
  }, 50);

  device.Stop();

  EXPECT_FALSE(got);
}

}  // namespace
}  // namespace shizuru::runtime
