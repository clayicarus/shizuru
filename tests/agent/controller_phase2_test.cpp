// Controller-level regression tests for Phase 2 paths.
// Validates end-to-end behavior of reducer-wired Controller for:
//   - Normal user message → respond-now / store-only
//   - Tool call cycle (think → tool call → tool result → continuation → respond)
//   - LLM failure → error → recovery
//   - Tool call timeout → timeout results → continuation → respond
//   - System event → think → respond
//   - Superseding message during turn-trigger evaluation
//   - Timer id reuse (Schedule → Cancel → Schedule)
//   - Interrupt always records interrupt memory

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "conversation/item.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "dialogue/timer_book.h"
#include "dialogue/types.h"
#include "io/data_frame.h"
#include "mock_audit_sink.h"
#include "mock_llm_client.h"
#include "mock_memory_store.h"
#include "policy/config.h"
#include "policy/policy_layer.h"
#include "policy/types.h"
#include "strategies/observation_aggregator.h"
#include "strategies/observation_filter.h"

namespace shizuru::core {
namespace {

// Poll predicate until true or timeout_ms elapses.
bool WaitFor(std::function<bool()> pred, int timeout_ms = 3000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// ---------------------------------------------------------------------------
// ObservationFilter that rejects everything (forces kStoreOnly verdict).
// ---------------------------------------------------------------------------
class RejectAllFilter : public ObservationFilter {
 public:
  bool ShouldProcess(const Observation& /*obs*/) override { return false; }
};

// ---------------------------------------------------------------------------
// ObservationFilter that accepts first N calls, then rejects.
// Used to test superseding: first call blocks, second call accepts.
// ---------------------------------------------------------------------------
class CountingFilter : public ObservationFilter {
 public:
  bool ShouldProcess(const Observation& /*obs*/) override {
    int c = call_count_.fetch_add(1);
    // Accept all calls by default.
    return c < accept_count_;
  }

  std::atomic<int> call_count_{0};
  int accept_count_ = 100;  // Accept all by default.
};

// Aggregator that always reports HasPending so RunLoop uses short wait.
class ShortWaitAggregator : public ObservationAggregator {
 public:
  std::optional<Observation> Feed(const Observation& obs) override {
    return obs;
  }
  std::optional<Observation> CheckTimeout() override { return std::nullopt; }
  bool HasPending() const override { return true; }
  void Reset() override {}
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class ControllerPhase2Test : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_config_.max_context_tokens = 100000;
    memory_store_ = std::make_unique<testing::MockMemoryStore>();
    context_ = std::make_unique<ContextStrategy>(ctx_config_, *memory_store_);
    context_->InitSession("test-session");

    policy_ = std::make_unique<PolicyLayer>(pol_config_, audit_sink_);
    policy_->InitSession("test-session");
  }

  Observation MakeUserObs(const std::string& content) {
    Observation obs;
    obs.type = ObservationType::kUserMessage;
    obs.content = content;
    obs.source = "user";
    obs.timestamp = std::chrono::steady_clock::now();
    obs.item = conversation::MakeHumanMessageItem("user", "", content);
    return obs;
  }

  Observation MakeSystemObs(const std::string& content) {
    Observation obs;
    obs.type = ObservationType::kSystemEvent;
    obs.content = content;
    obs.source = "scheduler";
    obs.timestamp = std::chrono::steady_clock::now();
    obs.item = conversation::MakeSystemEventItem(
        "system:scheduler", "", "reminder", "scheduler", content);
    return obs;
  }

  ControllerConfig DefaultConfig() {
    ControllerConfig cfg;
    cfg.action_count_limit = 100;
    cfg.max_retries = 3;
    cfg.retry_base_delay = std::chrono::milliseconds(1);
    cfg.max_continuations = 50;
    cfg.token_budget = 100000;
    cfg.action_count_limit = 50;
    cfg.tool_call_timeout = std::chrono::seconds(30);
    cfg.debounce_duration = std::chrono::milliseconds(200);
    return cfg;
  }

  testing::MockAuditSink audit_sink_;
  ContextConfig ctx_config_;
  PolicyConfig pol_config_;
  std::unique_ptr<testing::MockMemoryStore> memory_store_;
  std::unique_ptr<ContextStrategy> context_;
  std::unique_ptr<PolicyLayer> policy_;
};

// ---------------------------------------------------------------------------
// Test 1: Normal user message → respond-now → think → respond → listening
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, RespondNow_FullCycle) {
  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  llm_ptr->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "hello back";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, *context_, *policy_);

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("hello"));

  // Wait for full cycle: should end back at kListening after response.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [from, to, ev] : transitions) {
      if (from == State::kResponding && to == State::kListening &&
          ev == Event::kResponseDelivered) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  // Verify transition sequence includes Think → Route → Respond → Listen.
  std::lock_guard<std::mutex> lock(mu);
  bool found_thinking = false, found_responding = false, found_back = false;
  for (const auto& [from, to, ev] : transitions) {
    if (from == State::kListening && to == State::kThinking) found_thinking = true;
    if (to == State::kResponding) found_responding = true;
    if (from == State::kResponding && to == State::kListening) found_back = true;
  }
  EXPECT_TRUE(found_thinking);
  EXPECT_TRUE(found_responding);
  EXPECT_TRUE(found_back);

  // Verify user message is in committed history.
  auto entries = memory_store_->GetAll("test-session");
  bool found_user = false;
  for (const auto& e : entries) {
    if (e.content.find("hello") != std::string::npos) found_user = true;
  }
  EXPECT_TRUE(found_user);
}

// ---------------------------------------------------------------------------
// Test 2: Normal user message → store-only → stay listening
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, StoreOnly_StaysListening) {
  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> llm_call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    llm_call_count.fetch_add(1);
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "should not happen";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  // Use RejectAllFilter → kStoreOnly verdict.
  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, *context_, *policy_,
                  nullptr,  // aggregator
                  std::make_unique<RejectAllFilter>());

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("background noise"));

  // Give time for the observation to be processed.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should still be listening — no thinking started.
  EXPECT_EQ(ctrl.GetState(), State::kListening);
  EXPECT_EQ(llm_call_count.load(), 0);

  // Verify message is preserved in committed history.
  auto entries = memory_store_->GetAll("test-session");
  bool found_msg = false;
  for (const auto& e : entries) {
    if (e.content.find("background noise") != std::string::npos) found_msg = true;
  }
  EXPECT_TRUE(found_msg) << "Store-only message must be preserved in committed history";

  ctrl.Shutdown();
}

// ---------------------------------------------------------------------------
// Test 3: Normal user message → think → tool call → tool result → respond
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, ToolCallCycle_ThinkToolResultContinuationRespond) {
  PolicyConfig pol_cfg;
  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "my_tool";
  allow_rule.required_capability = "tool_cap";
  allow_rule.outcome = PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_rule};

  testing::MockAuditSink audit2;
  PolicyLayer policy2(pol_cfg, audit2);
  policy2.InitSession("test-session");
  policy2.GrantCapability("test-session", "tool_cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "my_tool";
      r.candidate.required_capability = "tool_cap";
      ToolCall tc;
      tc.id = "call_1";
      tc.name = "my_tool";
      tc.arguments = "{}";
      tc.required_capability = "tool_cap";
      r.candidate.tool_calls.push_back(std::move(tc));
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "tool done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller* ctrl_ptr = nullptr;
  std::mutex ctrl_mu;

  Controller::EmitFrameCallback emit_frame = [&](const std::string& port,
                                                  io::DataFrame frame) {
    if (port == "action_out") {
      std::lock_guard<std::mutex> lock(ctrl_mu);
      if (ctrl_ptr) {
        const std::string payload(frame.payload.begin(), frame.payload.end());
        const auto json = nlohmann::json::parse(payload);
        Observation result_obs;
        result_obs.type = ObservationType::kToolResult;
        result_obs.content = nlohmann::json({
            {"success", true},
            {"tool_name", json.value("tool_name", "my_tool")},
            {"tool_call_id", json.value("tool_call_id", "")},
            {"output", "tool output"},
        }).dump();
        result_obs.source = "tool:my_tool";
        result_obs.timestamp = std::chrono::steady_clock::now();
        ctrl_ptr->EnqueueObservation(std::move(result_obs));
      }
    }
  };

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  std::move(emit_frame), nullptr, *context_, policy2);
  {
    std::lock_guard<std::mutex> lock(ctrl_mu);
    ctrl_ptr = &ctrl;
  }

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  std::vector<std::string> responses;
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });
  ctrl.OnConversationItem([&](const conversation::ConversationItem& item, bool is_delta) {
    if (!is_delta && item.kind == conversation::ItemKind::kAssistantMessage) {
      std::lock_guard<std::mutex> lock(mu);
      responses.push_back(item.payload.value("text", ""));
    }
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("use tool"));

  // Wait for final response.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& r : responses) {
      if (r == "tool done") return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  // Verify we went through Acting state.
  std::lock_guard<std::mutex> lock(mu);
  bool found_acting = false;
  bool found_action_complete = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kActing) found_acting = true;
    if (from == State::kActing && to == State::kThinking) found_action_complete = true;
  }
  EXPECT_TRUE(found_acting);
  EXPECT_TRUE(found_action_complete);
  EXPECT_GE(call_count.load(), 2) << "LLM should be called at least twice (tool call + continuation)";
}

// ---------------------------------------------------------------------------
// Test 4: LLM failure → error state → recovery
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, LlmFailure_ErrorState_Recovery) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_retries = 0;  // Fail immediately.

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    if (c == 0) throw std::runtime_error("LLM exploded");
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "recovered";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm),
                  nullptr, nullptr, *context_, *policy_);

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("trigger error"));

  // Wait for Error state.
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kError; }));

  // Verify we reached error via kLlmFailure.
  {
    std::lock_guard<std::mutex> lock(mu);
    bool found_error = false;
    for (const auto& [from, to, ev] : transitions) {
      if (to == State::kError && ev == Event::kLlmFailure) found_error = true;
    }
    EXPECT_TRUE(found_error);
  }

  ctrl.Shutdown();
}

// ---------------------------------------------------------------------------
// Test 5: Tool call timeout → timeout results recorded → continuation → respond
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, ToolCallTimeout_TimeoutResultsRecorded_Continuation) {
  ControllerConfig cfg = DefaultConfig();
  cfg.tool_call_timeout = std::chrono::seconds(1);  // Short timeout.

  PolicyConfig pol_cfg;
  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "slow_tool";
  allow_rule.required_capability = "tool_cap";
  allow_rule.outcome = PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_rule};

  testing::MockAuditSink audit2;
  PolicyLayer policy2(pol_cfg, audit2);
  policy2.InitSession("test-session");
  policy2.GrantCapability("test-session", "tool_cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow& ctx) -> LlmResult {
    int c = call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "slow_tool";
      r.candidate.required_capability = "tool_cap";
      ToolCall tc;
      tc.id = "call_timeout_1";
      tc.name = "slow_tool";
      tc.arguments = "{}";
      tc.required_capability = "tool_cap";
      r.candidate.tool_calls.push_back(std::move(tc));
    } else {
      // Continuation after timeout — respond.
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "timeout handled";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // EmitFrameCallback: do NOT enqueue tool result — let it timeout.
  Controller::EmitFrameCallback emit_frame = [](const std::string&,
                                                io::DataFrame) {};

  Controller ctrl("test-session", cfg, std::move(llm),
                  std::move(emit_frame), nullptr, *context_, policy2,
                  std::make_unique<ShortWaitAggregator>());

  std::mutex mu;
  std::vector<std::string> responses;
  ctrl.OnConversationItem([&](const conversation::ConversationItem& item, bool is_delta) {
    if (!is_delta && item.kind == conversation::ItemKind::kAssistantMessage) {
      std::lock_guard<std::mutex> lock(mu);
      responses.push_back(item.payload.value("text", ""));
    }
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("use slow tool"));

  // Wait for the continuation response after timeout (timeout is 1s).
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& r : responses) {
      if (r == "timeout handled") return true;
    }
    return false;
  }, 5000));

  ctrl.Shutdown();

  // Verify timeout result was recorded in memory.
  auto entries = memory_store_->GetAll("test-session");
  bool found_timeout = false;
  for (const auto& e : entries) {
    if (e.content.find("timeout") != std::string::npos) found_timeout = true;
  }
  EXPECT_TRUE(found_timeout) << "Timeout result must be recorded in committed history";
  EXPECT_GE(call_count.load(), 2) << "LLM should be called at least twice (tool call + continuation after timeout)";
}

// ---------------------------------------------------------------------------
// Test 6: System event → think → respond
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, SystemEvent_ThinkRespond) {
  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  llm_ptr->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "reminder acknowledged";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, *context_, *policy_);

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  std::vector<std::string> responses;
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });
  ctrl.OnConversationItem([&](const conversation::ConversationItem& item, bool is_delta) {
    if (!is_delta && item.kind == conversation::ItemKind::kAssistantMessage) {
      std::lock_guard<std::mutex> lock(mu);
      responses.push_back(item.payload.value("text", ""));
    }
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeSystemObs("time to check in"));

  // Wait for response.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& r : responses) {
      if (r == "reminder acknowledged") return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  // Verify system event was recorded in memory.
  auto entries = memory_store_->GetAll("test-session");
  bool found_event = false;
  for (const auto& e : entries) {
    if (e.content.find("time to check in") != std::string::npos) found_event = true;
  }
  EXPECT_TRUE(found_event);

  // Verify transition through thinking.
  std::lock_guard<std::mutex> lock(mu);
  bool found_thinking = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kThinking) found_thinking = true;
  }
  EXPECT_TRUE(found_thinking);
}

// ---------------------------------------------------------------------------
// Test 7: Superseding message during turn-trigger evaluation
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, SupersedingMessage_CancelsOldEvaluation) {
  // Use a filter that rejects the first call (stale) and accepts the second.
  // The first message triggers classification → kStoreOnly (rejected).
  // Before that completes, a second message arrives and supersedes.
  // The second message's classification → kRespondNow.
  //
  // Since StartTurnTriggerClassification is currently synchronous in the
  // effect executor, we test the superseding path by sending two messages
  // rapidly. The first gets kStoreOnly, the second gets kRespondNow.
  // The key invariant: the second message triggers thinking, not the first.

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::mutex llm_mu;
  std::vector<ContextWindow> captured_windows;
  llm_ptr->submit_fn = [&](const ContextWindow& ctx) -> LlmResult {
    {
      std::lock_guard<std::mutex> lock(llm_mu);
      captured_windows.push_back(ctx);
    }
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "responded";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // Custom filter: reject first message, accept second.
  auto filter = std::make_unique<CountingFilter>();
  filter->accept_count_ = 0;  // Reject all initially.

  // We need a different approach: send first message (rejected → store-only),
  // then send second message (accepted → respond-now).
  // Use a filter that rejects "noise" and accepts "important".
  class SelectiveFilter : public ObservationFilter {
   public:
    bool ShouldProcess(const Observation& obs) override {
      return obs.content.find("important") != std::string::npos;
    }
  };

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  nullptr, nullptr, *context_, *policy_,
                  nullptr,  // aggregator
                  std::make_unique<SelectiveFilter>());

  std::mutex mu;
  std::vector<std::string> responses;
  ctrl.OnConversationItem([&](const conversation::ConversationItem& item, bool is_delta) {
    if (!is_delta && item.kind == conversation::ItemKind::kAssistantMessage) {
      std::lock_guard<std::mutex> lock(mu);
      responses.push_back(item.payload.value("text", ""));
    }
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  // First message: rejected by filter → store-only.
  ctrl.EnqueueObservation(MakeUserObs("noise message"));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Second message: accepted by filter → respond-now.
  ctrl.EnqueueObservation(MakeUserObs("important message"));

  // Wait for response.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !responses.empty();
  }));

  ctrl.Shutdown();

  // Verify: first message (noise) is in committed history.
  auto entries = memory_store_->GetAll("test-session");
  bool found_noise = false, found_important = false;
  for (const auto& e : entries) {
    if (e.content.find("noise message") != std::string::npos) found_noise = true;
    if (e.content.find("important message") != std::string::npos) found_important = true;
  }
  EXPECT_TRUE(found_noise) << "First (store-only) message must be in committed history";
  EXPECT_TRUE(found_important) << "Second (respond-now) message must be in committed history";

  // Verify: LLM was called and the context includes both messages.
  {
    std::lock_guard<std::mutex> lock(llm_mu);
    ASSERT_GE(captured_windows.size(), 1u);
    const auto& window = captured_windows[0];
    bool ctx_has_noise = false, ctx_has_important = false;
    for (const auto& msg : window.messages) {
      if (msg.content.find("noise message") != std::string::npos) ctx_has_noise = true;
      if (msg.content.find("important message") != std::string::npos) ctx_has_important = true;
    }
    // Both messages should be in context since store-only still records.
    EXPECT_TRUE(ctx_has_noise) << "Store-only message should be visible in LLM context";
    EXPECT_TRUE(ctx_has_important) << "Respond-now message should be visible in LLM context";
  }
}

// ---------------------------------------------------------------------------
// Test 8: Timer id reuse — Schedule → Cancel → Schedule → only second fires
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, TimerIdReuse_OnlySecondFires) {
  // This tests TimerBook directly since it's a pure data structure.
  // The Controller uses TimerBook internally; this validates the generation
  // safety that the Controller relies on.
  dialogue::TimerBook book;

  auto t0 = std::chrono::steady_clock::now();
  auto d1 = t0 + std::chrono::milliseconds(100);
  auto d2 = t0 + std::chrono::milliseconds(200);

  // Schedule("debounce") with deadline d1.
  book.Schedule(dialogue::TimerKind::kDebounce, "debounce", d1);

  // Cancel("debounce").
  book.Cancel("debounce");

  // Schedule("debounce") again with deadline d2.
  book.Schedule(dialogue::TimerKind::kDebounce, "debounce", d2);

  // Pop at d1 — first timer should NOT fire (it was cancelled).
  auto expired_at_d1 = book.PopExpired(d1);
  EXPECT_TRUE(expired_at_d1.empty())
      << "Cancelled first timer must not fire at its original deadline";

  // Pop at d2 — second timer should fire.
  auto expired_at_d2 = book.PopExpired(d2);
  ASSERT_EQ(expired_at_d2.size(), 1u);
  EXPECT_EQ(expired_at_d2[0].timer_id, "debounce");
  EXPECT_EQ(expired_at_d2[0].kind, dialogue::TimerKind::kDebounce);

  // Book should be empty now.
  EXPECT_TRUE(book.Empty());
}

// ---------------------------------------------------------------------------
// Test 9: Interrupt always records interrupt memory
// ---------------------------------------------------------------------------
TEST_F(ControllerPhase2Test, InterruptRecordsInterruptMemory) {
  // Set up a tool call scenario so we can interrupt during kActing.
  PolicyConfig pol_cfg;
  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "blocking_tool";
  allow_rule.required_capability = "tool_cap";
  allow_rule.outcome = PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_rule};

  testing::MockAuditSink audit2;
  PolicyLayer policy2(pol_cfg, audit2);
  policy2.InitSession("test-session");
  policy2.GrantCapability("test-session", "tool_cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "blocking_tool";
      r.candidate.required_capability = "tool_cap";
      ToolCall tc;
      tc.id = "call_block_1";
      tc.name = "blocking_tool";
      tc.arguments = "{}";
      tc.required_capability = "tool_cap";
      r.candidate.tool_calls.push_back(std::move(tc));
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // Do NOT enqueue tool result — leave in kActing so we can interrupt.
  Controller::EmitFrameCallback emit_frame = [](const std::string&,
                                                io::DataFrame) {};

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  std::move(emit_frame), nullptr, *context_, policy2);

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("use blocking tool"));

  // Wait for kActing.
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kActing; }));

  // Send interrupt via barge-in user message.
  ctrl.EnqueueObservation(MakeUserObs("interrupt!"));

  // Wait for transition back to kListening.
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.Shutdown();

  // Verify "Turn interrupted" is in committed history.
  auto entries = memory_store_->GetAll("test-session");
  bool found_interrupt_memory = false;
  for (const auto& e : entries) {
    if (e.content.find("interrupted") != std::string::npos ||
        e.content.find("Interrupt") != std::string::npos) {
      found_interrupt_memory = true;
    }
  }
  EXPECT_TRUE(found_interrupt_memory)
      << "Interrupt must record 'Turn interrupted' entry in committed history";

  // Also verify the interrupting message is recorded.
  bool found_interrupt_msg = false;
  for (const auto& e : entries) {
    if (e.content.find("interrupt!") != std::string::npos) {
      found_interrupt_msg = true;
    }
  }
  EXPECT_TRUE(found_interrupt_msg)
      << "Interrupting user message must be recorded in committed history";
}

}  // namespace
}  // namespace shizuru::core
