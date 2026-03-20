// Unit tests for CoreDevice
// Uses Google Test

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controller/config.h"
#include "context/config.h"
#include "policy/config.h"
#include "controller/types.h"
#include "context/types.h"
#include "io/data_frame.h"
#include "runtime/core_device.h"
#include "mock_audit_sink.h"
#include "mock_io_bridge.h"
#include "mock_llm_client.h"
#include "mock_memory_store.h"

namespace shizuru::runtime {
namespace {

// ---------------------------------------------------------------------------
// Helper: build a CoreDevice with mock dependencies
// ---------------------------------------------------------------------------

std::unique_ptr<CoreDevice> MakeCoreDevice(
    const std::string& device_id,
    core::testing::MockLlmClient** llm_out = nullptr,
    core::testing::MockIoBridge** io_out = nullptr,
    core::PolicyConfig pol_cfg = {}) {
  auto llm = std::make_unique<core::testing::MockLlmClient>();
  auto io  = std::make_unique<core::testing::MockIoBridge>();
  if (llm_out) *llm_out = llm.get();
  if (io_out)  *io_out  = io.get();

  // Default LLM: return a kResponse to end the loop quickly.
  llm->submit_fn = [](const core::ContextWindow&) -> core::LlmResult {
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "done";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  core::ControllerConfig ctrl_cfg;
  ctrl_cfg.max_turns = 5;
  ctrl_cfg.max_retries = 0;
  ctrl_cfg.retry_base_delay = std::chrono::milliseconds(1);
  ctrl_cfg.wall_clock_timeout = std::chrono::seconds(5);
  ctrl_cfg.token_budget = 100000;
  ctrl_cfg.action_count_limit = 10;

  core::ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;

  return std::make_unique<CoreDevice>(
      device_id, "test-session",
      ctrl_cfg, ctx_cfg, std::move(pol_cfg),
      std::move(llm), std::move(io),
      std::make_unique<core::testing::MockMemoryStore>(),
      std::make_unique<core::testing::MockAuditSink>());
}

// Wait up to `timeout_ms` for `predicate` to become true, polling every 5ms.
bool WaitFor(std::function<bool()> predicate, int timeout_ms = 300) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

// ---------------------------------------------------------------------------
// Test 1: TextMessageToObservationMapping
// Verify "hello world" text → MockLlmClient receives ContextWindow whose
// last message content is "hello world".
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, TextMessageToObservationMapping) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_unit", &llm);

  std::mutex mu;
  std::vector<core::ContextWindow> windows;
  llm->submit_fn = [&](const core::ContextWindow& cw) -> core::LlmResult {
    {
      std::lock_guard<std::mutex> lock(mu);
      windows.push_back(cw);
    }
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "done";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});
  device->Start();

  const std::string text = "hello world";
  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload = std::vector<uint8_t>(text.begin(), text.end());
  device->OnInput("text_in", std::move(frame));

  bool got_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !windows.empty();
  });

  device->Stop();

  ASSERT_TRUE(got_call) << "MockLlmClient was never called";
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_FALSE(windows.empty());
  const auto& msgs = windows.front().messages;
  ASSERT_FALSE(msgs.empty());
  EXPECT_EQ(msgs.back().content, text);
}

// ---------------------------------------------------------------------------
// Test 2: ToolCallActionCandidateToDataFrame
// Configure LLM to return kToolCall → verify emitted DataFrame has
// type="action/tool_call".
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, ToolCallActionCandidateToDataFrame) {
  core::testing::MockLlmClient* llm = nullptr;

  // Policy must allow the "search" tool call or the controller will deny it.
  core::PolicyConfig pol_cfg;
  core::PolicyRule allow_search;
  allow_search.priority = 0;
  allow_search.action_pattern = "search";
  allow_search.required_capability = "search";
  allow_search.outcome = core::PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_search};
  pol_cfg.default_capabilities = {"search"};

  auto device = MakeCoreDevice("core_unit2", &llm, nullptr, std::move(pol_cfg));

  // First call: return kToolCall. Second call (after tool result): kResponse.
  std::atomic<int> call_count{0};
  llm->submit_fn = [&](const core::ContextWindow&) -> core::LlmResult {
    int n = call_count.fetch_add(1);
    core::LlmResult r;
    if (n == 0) {
      r.candidate.type = core::ActionType::kToolCall;
      r.candidate.action_name = "search";
      r.candidate.arguments = "{}";
    } else {
      r.candidate.type = core::ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  std::mutex mu;
  std::vector<io::DataFrame> emitted;
  device->SetOutputCallback([&](const std::string&, const std::string&,
                                 io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.push_back(std::move(f));
  });

  device->Start();

  const std::string trigger = "go";
  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload = std::vector<uint8_t>(trigger.begin(), trigger.end());
  device->OnInput("text_in", std::move(frame));

  bool got_tool_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& f : emitted) {
      if (f.type == "action/tool_call") return true;
    }
    return false;
  });

  device->Stop();

  ASSERT_TRUE(got_tool_call) << "No action/tool_call DataFrame was emitted";
  std::lock_guard<std::mutex> lock(mu);
  bool found = false;
  for (const auto& f : emitted) {
    if (f.type == "action/tool_call") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Test 3: UnsupportedTypeDiscarded
// OnInput("video_in", frame) → no output callback, no crash.
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, UnsupportedTypeDiscarded) {
  auto device = MakeCoreDevice("core_unit3");

  std::atomic<int> callback_count{0};
  device->SetOutputCallback([&](const std::string&, const std::string&,
                                 io::DataFrame) {
    ++callback_count;
  });

  device->Start();

  io::DataFrame frame;
  frame.type = "video/mp4";
  const std::string payload = "fake-video-data";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());

  EXPECT_NO_THROW(device->OnInput("video_in", std::move(frame)));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  device->Stop();

  EXPECT_EQ(callback_count.load(), 0);
}

// ---------------------------------------------------------------------------
// Test 4: ToolResultInPortCreatesToolResultObservation
// OnInput("tool_result_in", frame) → MockLlmClient eventually receives a
// ContextWindow (the tool result was enqueued and processed).
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, ToolResultInPortCreatesToolResultObservation) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_unit4", &llm);

  std::mutex mu;
  std::vector<core::ContextWindow> windows;
  // First LLM call (triggered by user message): return kToolCall for "search".
  // Second LLM call (after tool result arrives): return kResponse to end turn.
  std::atomic<int> call_count{0};
  llm->submit_fn = [&](const core::ContextWindow& cw) -> core::LlmResult {
    {
      std::lock_guard<std::mutex> lock(mu);
      windows.push_back(cw);
    }
    int n = call_count.fetch_add(1);
    core::LlmResult r;
    if (n == 0) {
      // First call: emit a tool call so the controller waits for a tool result.
      r.candidate.type = core::ActionType::kToolCall;
      r.candidate.action_name = "search";
      r.candidate.arguments = "{}";
    } else {
      r.candidate.type = core::ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  // Policy must allow "search" so the tool call is not denied.
  core::PolicyConfig pol_cfg;
  core::PolicyRule allow_search;
  allow_search.priority = 0;
  allow_search.action_pattern = "search";
  allow_search.required_capability = "search";
  allow_search.outcome = core::PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_search};
  pol_cfg.default_capabilities = {"search"};

  // Rebuild device with the policy config.
  device = MakeCoreDevice("core_unit4b", &llm, nullptr, std::move(pol_cfg));
  call_count.store(0);
  {
    std::lock_guard<std::mutex> lock(mu);
    windows.clear();
  }
  llm->submit_fn = [&](const core::ContextWindow& cw) -> core::LlmResult {
    {
      std::lock_guard<std::mutex> lock(mu);
      windows.push_back(cw);
    }
    int n = call_count.fetch_add(1);
    core::LlmResult r;
    if (n == 0) {
      r.candidate.type = core::ActionType::kToolCall;
      r.candidate.action_name = "search";
      r.candidate.arguments = "{}";
    } else {
      r.candidate.type = core::ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});
  device->Start();

  // Step 1: send a user message to start a turn; the LLM will return kToolCall.
  const std::string user_msg = "trigger";
  io::DataFrame user_frame;
  user_frame.type = "text/plain";
  user_frame.payload = std::vector<uint8_t>(user_msg.begin(), user_msg.end());
  device->OnInput("text_in", std::move(user_frame));

  // Wait for the first LLM call (kToolCall) to be processed.
  bool first_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !windows.empty();
  });
  ASSERT_TRUE(first_call) << "LLM was never called for user message";

  // Step 2: now send the tool result — the controller is waiting for it.
  const std::string result_payload = "{\"result\": \"ok\"}";
  io::DataFrame frame;
  frame.type = "action/tool_result";
  frame.payload = std::vector<uint8_t>(result_payload.begin(),
                                        result_payload.end());
  device->OnInput("tool_result_in", std::move(frame));

  // The second LLM call should happen after the tool result is enqueued.
  bool second_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return windows.size() >= 2;
  });

  device->Stop();

  EXPECT_TRUE(second_call) << "LLM was not called again after tool_result_in";
}

// ---------------------------------------------------------------------------
// Test 5: StoppedDeviceDiscardsFrames
// Stop() then OnInput("text_in", frame) → MockLlmClient NOT called.
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, StoppedDeviceDiscardsFrames) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_unit5", &llm);

  std::atomic<int> submit_count{0};
  llm->submit_fn = [&](const core::ContextWindow&) -> core::LlmResult {
    ++submit_count;
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "done";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});
  device->Start();
  device->Stop();

  // Reset counter after stop (the session may have processed a startup cycle).
  submit_count.store(0);

  const std::string text = "should be discarded";
  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload = std::vector<uint8_t>(text.begin(), text.end());
  EXPECT_NO_THROW(device->OnInput("text_in", std::move(frame)));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_EQ(submit_count.load(), 0)
      << "LLM was called after device was stopped";
}

}  // namespace
}  // namespace shizuru::runtime
