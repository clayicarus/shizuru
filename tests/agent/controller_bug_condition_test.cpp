// Bug condition exploration tests for Controller
// These tests are EXPECTED TO FAIL on unfixed code — failure confirms bugs exist.
// DO NOT fix the tests or the code when they fail.
//
// Bug 1: Interrupt during streaming — Interrupt() never calls llm_->Cancel()
// Bug 2: kContinue deadlock — HandleRouting doesn't re-enter HandleThinking

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "io/data_frame.h"
#include "mock_audit_sink.h"
#include "mock_llm_client.h"
#include "mock_memory_store.h"
#include "policy/config.h"
#include "policy/policy_layer.h"

namespace shizuru::core {
namespace {

// Poll predicate until true or timeout_ms elapses.
bool WaitFor(std::function<bool()> pred, int timeout_ms = 2000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

Observation MakeUserObs(const std::string& content) {
  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = content;
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  return obs;
}

ControllerConfig DefaultConfig() {
  ControllerConfig cfg;
  cfg.max_turns = 20;
  cfg.max_retries = 3;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.turn_timeout = std::chrono::seconds(10);
  cfg.token_budget = 100000;
  cfg.action_count_limit = 50;
  return cfg;
}

// ---------------------------------------------------------------------------
// Bug 1 — Interrupt During Streaming
// ---------------------------------------------------------------------------
// On unfixed code, Controller::Interrupt() only enqueues an observation and
// never calls llm_->Cancel(). When the loop thread is blocked inside
// SubmitStreaming, the interrupt observation sits in the queue until the LLM
// call completes on its own. This test confirms the bug by asserting that
// cancel_count >= 1 within 200ms of calling Interrupt() — which will FAIL
// on unfixed code because Cancel() is never called.
//
// **Validates: Requirements 1.1, 1.2**
// ---------------------------------------------------------------------------
TEST(ControllerBugCondition, Bug1_InterruptDuringStreaming_CancelNotCalled) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("test");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("test");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  // submit_fn blocks for 2 seconds, simulating SubmitStreaming blocking the
  // loop thread. This means RunLoop cannot dequeue the interrupt observation
  // until this returns.
  llm_ptr->submit_fn = [](const ContextWindow&) -> LlmResult {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "done";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  // use_streaming = true so Controller calls SubmitStreaming (which invokes
  // submit_fn in MockLlmClient).
  ControllerConfig cfg = DefaultConfig();
  cfg.use_streaming = true;

  Controller ctrl("test", cfg, std::move(llm), nullptr, nullptr,
                  context, policy);

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  // Send a user message to trigger the thinking→SubmitStreaming path.
  ctrl.EnqueueObservation(MakeUserObs("hello"));

  // Wait for the controller to enter kThinking (loop thread now blocked in
  // SubmitStreaming for ~2 seconds).
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kThinking; }, 500));

  // Give a moment for SubmitStreaming to actually start blocking.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Call Interrupt() from this thread (simulating speech_start on CoreDevice thread).
  ctrl.Interrupt();

  // Assert: llm_->Cancel() should have been called within 200ms.
  // On UNFIXED code, Interrupt() only enqueues an observation — Cancel() is
  // never called, so cancel_count stays 0. This assertion FAILS, confirming
  // Bug 1 exists.
  bool cancelled = WaitFor([&] { return llm_ptr->cancel_count >= 1; }, 200);

  // Shut down (the 2-second sleep will eventually complete).
  ctrl.Shutdown();

  EXPECT_TRUE(cancelled)
      << "Bug 1 confirmed: Interrupt() did not call llm_->Cancel() — "
         "cancel_count=" << llm_ptr->cancel_count;
}

// ---------------------------------------------------------------------------
// Bug 2 — kContinue Deadlock
// ---------------------------------------------------------------------------
// On unfixed code, when the LLM returns ActionType::kContinue, HandleRouting
// transitions to kThinking via kRouteToContinue but never calls HandleThinking()
// and never enqueues a continuation observation. RunLoop blocks on
// queue_cv_.wait_for() waiting for an observation that never arrives —
// the controller deadlocks.
//
// This test sends one user observation, where the first LLM call returns
// kContinue and the second returns kResponse with text "done". It waits for
// the response callback to fire within 2 seconds. On unfixed code, the
// controller deadlocks after kContinue and the response never arrives.
//
// **Validates: Requirements 1.3, 1.4**
// ---------------------------------------------------------------------------
TEST(ControllerBugCondition, Bug2_kContinueDeadlock_ResponseNeverArrives) {
  testing::MockMemoryStore memory;
  testing::MockAuditSink audit;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory);
  context.InitSession("test");
  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit);
  policy.InitSession("test");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  // First call returns kContinue, second returns kResponse.
  // use_streaming = false so Controller calls Submit (not SubmitStreaming).
  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kContinue;
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  ControllerConfig cfg = DefaultConfig();
  cfg.use_streaming = false;

  Controller ctrl("test", cfg, std::move(llm), nullptr, nullptr,
                  context, policy);

  // Track response callback.
  std::atomic<bool> response_received{false};
  std::string response_text;
  std::mutex mu;
  ctrl.OnResponse([&](const ActionCandidate& ac) {
    std::lock_guard<std::mutex> lock(mu);
    response_text = ac.response_text;
    response_received.store(true);
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  // Send one user observation.
  ctrl.EnqueueObservation(MakeUserObs("trigger continue"));

  // Wait for the response callback to fire within 2 seconds.
  // On UNFIXED code, HandleRouting transitions to kThinking on kContinue but
  // never calls HandleThinking() — the controller deadlocks and the response
  // never arrives. This assertion FAILS, confirming Bug 2 exists.
  bool got_response = WaitFor([&] { return response_received.load(); }, 2000);

  ctrl.Shutdown();

  EXPECT_TRUE(got_response)
      << "Bug 2 confirmed: kContinue deadlock — response callback never fired. "
         "LLM was called " << call_count.load() << " time(s)";
  if (got_response) {
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(response_text, "done");
  }
}

}  // namespace
}  // namespace shizuru::core
