// Unit tests for CoreDevice
// Uses Google Test

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "controller/config.h"
#include "context/config.h"
#include "conversation/item.h"
#include "policy/config.h"
#include "controller/types.h"
#include "context/types.h"
#include "io/control_frame.h"
#include "io/data_frame.h"
#include "io/interrupt_frame.h"
#include "io/io_device.h"
#include "runtime/core_device.h"
#include "mock_audit_sink.h"
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
    core::PolicyConfig pol_cfg = {}) {
  auto llm = std::make_unique<core::testing::MockLlmClient>();
  if (llm_out) *llm_out = llm.get();

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
  ctrl_cfg.max_retries = 0;
  ctrl_cfg.retry_base_delay = std::chrono::milliseconds(1);
  ctrl_cfg.token_budget = 100000;
  ctrl_cfg.action_count_limit = 10;

  core::ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;

  return std::make_unique<CoreDevice>(
      device_id, "test-session",
      ctrl_cfg, ctx_cfg, std::move(pol_cfg),
      std::move(llm),
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

  auto device = MakeCoreDevice("core_unit2", &llm, std::move(pol_cfg));

  // First call: return kToolCall. Second call (after tool result): kResponse.
  std::atomic<int> call_count{0};
  llm->submit_fn = [&](const core::ContextWindow&) -> core::LlmResult {
    int n = call_count.fetch_add(1);
    core::LlmResult r;
    if (n == 0) {
      r.candidate.type = core::ActionType::kToolCall;
      r.candidate.action_name = "search";
      r.candidate.arguments = "{}";
      {
        core::ToolCall tc;
        tc.id = "call_dev_1";
        tc.name = "search";
        tc.arguments = "{}";
        r.candidate.tool_calls.push_back(std::move(tc));
      }
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
      const std::string payload(f.payload.begin(), f.payload.end());
      const auto json = nlohmann::json::parse(payload);
      EXPECT_EQ(json.value("tool_name", ""), "search");
      ASSERT_TRUE(json.contains("arguments"));
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
      {
        core::ToolCall tc;
        tc.id = "call_dev_2";
        tc.name = "search";
        tc.arguments = "{}";
        r.candidate.tool_calls.push_back(std::move(tc));
      }
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
  device = MakeCoreDevice("core_unit4b", &llm, std::move(pol_cfg));
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
      {
        core::ToolCall tc;
        tc.id = "call_dev_3";
        tc.name = "search";
        tc.arguments = "{}";
        r.candidate.tool_calls.push_back(std::move(tc));
      }
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
  const std::string result_payload =
      R"({"success":true,"tool_name":"search","tool_call_id":"call_dev_3","output":"ok"})";
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
// Test: GetPortDescriptors contains interrupt_in and control_out with correct types
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, GetPortDescriptorsContainsInterruptInAndControlOut) {
  auto device = MakeCoreDevice("core_ports");

  const auto ports = device->GetPortDescriptors();

  bool found_interrupt_in = false;
  bool found_control_out = false;
  for (const auto& p : ports) {
    if (p.name == "interrupt_in") {
      EXPECT_EQ(p.direction, io::PortDirection::kInput);
      EXPECT_EQ(p.data_type, io::InterruptFrame::kType);
      found_interrupt_in = true;
    }
    if (p.name == "control_out") {
      EXPECT_EQ(p.direction, io::PortDirection::kOutput);
      EXPECT_EQ(p.data_type, "control/command");
      found_control_out = true;
    }
  }
  EXPECT_TRUE(found_interrupt_in)
      << "interrupt_in port not found in GetPortDescriptors()";
  EXPECT_TRUE(found_control_out) << "control_out port not found in GetPortDescriptors()";
}

// ---------------------------------------------------------------------------
// Test: interrupt/request on interrupt_in → control_out emits "cancel"
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, InterruptInEmitsCancelOnControlOut) {
  auto device = MakeCoreDevice("core_vad_unit");

  std::mutex mu;
  std::vector<std::pair<std::string, io::DataFrame>> emitted;
  device->SetOutputCallback([&](const std::string& /*dev*/,
                                const std::string& port,
                                io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.emplace_back(port, std::move(f));
  });

  device->Start();

  auto interrupt = io::InterruptFrame::Make(
      io::InterruptFrame::kReasonBargeIn, "voice");
  device->OnInput("interrupt_in", std::move(interrupt));

  bool got_cancel = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [port, f] : emitted) {
      if (port == "control_out" &&
          io::ControlFrame::Parse(f) == io::ControlFrame::kCommandCancel) {
        return true;
      }
    }
    return false;
  });

  device->Stop();

  EXPECT_TRUE(got_cancel)
      << "control_out did not emit 'cancel' after interrupt_in";
}

// ---------------------------------------------------------------------------
// Test: kResponseDelivered transition → control_out must NOT emit "cancel"
// (cancel is only emitted on kInterrupt, not on normal response delivery)
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, ResponseDeliveredTransitionDoesNotEmitCancel) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_resp_del", &llm);

  // LLM returns kResponse immediately — this triggers kResponseDelivered.
  llm->submit_fn = [](const core::ContextWindow&) -> core::LlmResult {
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "hello";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  // Register OnConversationItem to detect when the response is delivered.
  std::mutex conv_mu;
  std::string conversation_response;
  device->Session().GetController().OnConversationItem(
      [&](const core::conversation::ConversationItem& item, bool is_delta) {
        if (!is_delta && item.kind == core::conversation::ItemKind::kAssistantMessage) {
          std::lock_guard<std::mutex> lock(conv_mu);
          conversation_response = item.payload.value("text", "");
        }
      });

  std::mutex mu;
  std::vector<std::pair<std::string, io::DataFrame>> emitted;
  device->SetOutputCallback([&](const std::string& /*dev*/,
                                const std::string& port,
                                io::DataFrame f) {
    std::lock_guard<std::mutex> lock(mu);
    emitted.emplace_back(port, std::move(f));
  });

  device->Start();

  const std::string text = "trigger";
  io::DataFrame frame;
  frame.type = "text/plain";
  frame.payload = std::vector<uint8_t>(text.begin(), text.end());
  device->OnInput("text_in", std::move(frame));

  // Wait for OnConversationItem to fire (confirms the turn completed).
  bool got_response = WaitFor([&] {
    std::lock_guard<std::mutex> lock(conv_mu);
    return !conversation_response.empty();
  });

  // Give a brief window for any spurious cancel to appear.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  device->Stop();

  ASSERT_TRUE(got_response) << "OnConversationItem was never called with assistant response";
  {
    std::lock_guard<std::mutex> lock(conv_mu);
    EXPECT_EQ(conversation_response, "hello");
  }

  std::lock_guard<std::mutex> lock(mu);
  bool found_cancel = false;
  for (const auto& [port, f] : emitted) {
    if (port == "control_out" && io::ControlFrame::Parse(f) == "cancel") {
      found_cancel = true;
      break;
    }
  }
  EXPECT_FALSE(found_cancel) << "control_out must NOT emit 'cancel' on kResponseDelivered";
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

// ---------------------------------------------------------------------------
// Test: SchedulerIn passes content as user message with name="scheduler"
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, SchedulerEventUsesNameField) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_sched", &llm);

  std::mutex mu;
  std::vector<core::ContextWindow> windows;
  llm->submit_fn = [&](const core::ContextWindow& cw) -> core::LlmResult {
    {
      std::lock_guard<std::mutex> lock(mu);
      windows.push_back(cw);
    }
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "ok";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});
  device->Start();

  // Send a scheduler event with pure JSON payload.
  const std::string payload = R"({"message":"dentist appointment"})";
  io::DataFrame frame;
  frame.type = "scheduler/event";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());
  device->OnInput("scheduler_in", std::move(frame));

  bool got_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !windows.empty();
  }, 500);

  device->Stop();

  ASSERT_TRUE(got_call) << "LLM was never called for scheduler event";
  std::lock_guard<std::mutex> lock(mu);
  ASSERT_FALSE(windows.empty());

  // Find the scheduler message — role="user", name="scheduler" as a weak
  // hint, but the real event semantics live in the explicit <event> envelope.
  bool found = false;
  for (const auto& msg : windows.front().messages) {
    if (msg.name == "scheduler") {
      EXPECT_EQ(msg.role, "user");
      EXPECT_NE(msg.content.find("<event"), std::string::npos);
      EXPECT_NE(msg.content.find("event_type=\"reminder\""), std::string::npos);
      EXPECT_NE(msg.content.find("source=\"scheduler\""), std::string::npos);
      EXPECT_NE(msg.content.find("dentist appointment"), std::string::npos);
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "No message with name='scheduler' found in context window";
}

// ---------------------------------------------------------------------------
// Test: SchedulerIn source_tag propagates even without event_type metadata
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, SchedulerEventSourceTagAlwaysSet) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_sched_def", &llm);

  std::mutex mu;
  std::vector<core::ContextWindow> windows;
  llm->submit_fn = [&](const core::ContextWindow& cw) -> core::LlmResult {
    {
      std::lock_guard<std::mutex> lock(mu);
      windows.push_back(cw);
    }
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "ok";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});
  device->Start();

  // Send a scheduler event WITHOUT event_type metadata.
  const std::string payload = R"({"topic":"interview prep"})";
  io::DataFrame frame;
  frame.type = "scheduler/event";
  frame.payload = std::vector<uint8_t>(payload.begin(), payload.end());
  device->OnInput("scheduler_in", std::move(frame));

  bool got_call = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !windows.empty();
  }, 500);

  device->Stop();

  ASSERT_TRUE(got_call);
  std::lock_guard<std::mutex> lock(mu);

  bool found = false;
  for (const auto& msg : windows.front().messages) {
    if (msg.name == "scheduler") {
      EXPECT_NE(msg.content.find("<event"), std::string::npos);
      EXPECT_NE(msg.content.find("event_type=\"reminder\""), std::string::npos);
      EXPECT_NE(msg.content.find("interview prep"), std::string::npos);
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "name='scheduler' should be set regardless of metadata";
}

// ---------------------------------------------------------------------------
// Test: StreamingTokensEmittedWithMetadataFlag
// With use_streaming=true, SubmitStreaming is called and each token delta
// arrives via OnConversationItem with is_delta=true.
// The final response (is_delta=false) must also fire.
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, StreamingTokensEmittedWithMetadataFlag) {
  auto llm = std::make_unique<core::testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  // SubmitStreaming fires three token callbacks then returns a kResponse.
  llm_ptr->submit_fn = [](const core::ContextWindow&) -> core::LlmResult {
    // submit_fn is called by both Submit and SubmitStreaming in the mock.
    // We return kResponse so the controller routes to HandleResponding.
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "hello world";
    r.prompt_tokens = 5;
    r.completion_tokens = 3;
    return r;
  };

  // Override SubmitStreaming directly via a subclass so we can fire on_token.
  class StreamingMock : public core::testing::MockLlmClient {
   public:
    core::LlmResult SubmitStreaming(const core::ContextWindow& ctx,
                                    core::StreamCallback on_token) override {
      // Simulate three token chunks.
      if (on_token) {
        on_token("hel");
        on_token("lo ");
        on_token("world");
      }
      return submit_fn(ctx);
    }
  };

  auto streaming_llm = std::make_unique<StreamingMock>();
  streaming_llm->submit_fn = [](const core::ContextWindow&) -> core::LlmResult {
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "hello world";
    r.prompt_tokens = 5;
    r.completion_tokens = 3;
    return r;
  };

  core::ControllerConfig ctrl_cfg;
  ctrl_cfg.max_retries = 0;
  ctrl_cfg.retry_base_delay = std::chrono::milliseconds(1);
  ctrl_cfg.token_budget = 100000;
  ctrl_cfg.action_count_limit = 10;
  ctrl_cfg.use_streaming = true;  // ← enable streaming

  core::ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;

  auto device = std::make_unique<CoreDevice>(
      "core_streaming", "test-session-streaming",
      ctrl_cfg, ctx_cfg, core::PolicyConfig{},
      std::move(streaming_llm),
      std::make_unique<core::testing::MockMemoryStore>(),
      std::make_unique<core::testing::MockAuditSink>());

  // Register OnConversationItem to capture streaming deltas and final response.
  std::mutex mu;
  std::vector<std::pair<core::conversation::ConversationItem, bool>> items;
  device->Session().GetController().OnConversationItem(
      [&](const core::conversation::ConversationItem& item, bool is_delta) {
        if (item.kind == core::conversation::ItemKind::kAssistantMessage) {
          std::lock_guard<std::mutex> lock(mu);
          items.emplace_back(item, is_delta);
        }
      });

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});

  device->Start();

  io::DataFrame in_frame;
  in_frame.type = "text/plain";
  const std::string msg = "hi";
  in_frame.payload = std::vector<uint8_t>(msg.begin(), msg.end());
  device->OnInput("text_in", std::move(in_frame));

  // Wait for at least one streaming delta + the final response.
  bool got_all = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    // Expect 3 delta items + 1 final item.
    return items.size() >= 4;
  }, 500);

  device->Stop();

  ASSERT_TRUE(got_all) << "Did not receive expected conversation items in time";

  std::lock_guard<std::mutex> lock(mu);

  // Collect partial (delta) and final items.
  std::vector<std::string> partial_tokens;
  int final_count = 0;
  for (const auto& [item, is_delta] : items) {
    if (is_delta) {
      partial_tokens.push_back(item.payload.value("text", ""));
    } else {
      ++final_count;
    }
  }

  EXPECT_EQ(partial_tokens.size(), 3u) << "Expected 3 streaming delta items";
  EXPECT_EQ(final_count, 1) << "Expected exactly 1 final (non-delta) item";

  // Verify token content.
  EXPECT_EQ(partial_tokens[0], "hel");
  EXPECT_EQ(partial_tokens[1], "lo ");
  EXPECT_EQ(partial_tokens[2], "world");
}

// ---------------------------------------------------------------------------
// Test: NonStreamingPathDoesNotSetMetadataFlag
// With use_streaming=false (default), Submit is called and the response
// arrives via OnConversationItem with is_delta=false (no streaming deltas).
// ---------------------------------------------------------------------------
TEST(CoreDeviceTest, NonStreamingPathDoesNotSetMetadataFlag) {
  core::testing::MockLlmClient* llm = nullptr;
  auto device = MakeCoreDevice("core_no_stream", &llm);

  llm->submit_fn = [](const core::ContextWindow&) -> core::LlmResult {
    core::LlmResult r;
    r.candidate.type = core::ActionType::kResponse;
    r.candidate.response_text = "non-streaming response";
    r.prompt_tokens = 5;
    r.completion_tokens = 5;
    return r;
  };

  // Register OnConversationItem to capture items.
  std::mutex mu;
  std::vector<std::pair<core::conversation::ConversationItem, bool>> items;
  device->Session().GetController().OnConversationItem(
      [&](const core::conversation::ConversationItem& item, bool is_delta) {
        if (item.kind == core::conversation::ItemKind::kAssistantMessage) {
          std::lock_guard<std::mutex> lock(mu);
          items.emplace_back(item, is_delta);
        }
      });

  device->SetOutputCallback([](const std::string&, const std::string&,
                                io::DataFrame) {});

  device->Start();

  io::DataFrame in_frame;
  in_frame.type = "text/plain";
  const std::string msg = "hello";
  in_frame.payload = std::vector<uint8_t>(msg.begin(), msg.end());
  device->OnInput("text_in", std::move(in_frame));

  bool got_response = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !items.empty();
  });

  device->Stop();

  ASSERT_TRUE(got_response) << "No conversation item received";

  std::lock_guard<std::mutex> lock(mu);
  for (const auto& [item, is_delta] : items) {
    EXPECT_FALSE(is_delta)
        << "Non-streaming path must not produce delta conversation items";
  }
}

}  // namespace
}  // namespace shizuru::runtime
