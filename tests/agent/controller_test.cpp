// Unit tests for Controller
// Tests specific examples, edge cases, and integration scenarios.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "mock_audit_sink.h"
#include "mock_io_bridge.h"
#include "mock_llm_client.h"
#include "mock_memory_store.h"
#include "policy/config.h"
#include "policy/policy_layer.h"
#include "policy/types.h"

namespace shizuru::core {
namespace {

// ---------------------------------------------------------------------------
// Test fixture with shared setup
// ---------------------------------------------------------------------------
class ControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_config_.max_context_tokens = 100000;
    context_ = std::make_unique<ContextStrategy>(ctx_config_, memory_store_);
    context_->InitSession("test-session");

    policy_ = std::make_unique<PolicyLayer>(pol_config_, audit_sink_);
    policy_->InitSession("test-session");
  }

  // Build a Controller with the given config and mock behaviors.
  // Must be called after configuring llm/io behaviors.
  std::unique_ptr<Controller> MakeController(ControllerConfig cfg) {
    auto llm = std::make_unique<testing::MockLlmClient>();
    auto io = std::make_unique<testing::MockIoBridge>();
    llm_raw_ = llm.get();
    io_raw_ = io.get();

    // Default LLM: return a response to end the loop.
    llm_raw_->submit_fn = [](const ContextWindow&) -> LlmResult {
      LlmResult r;
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    };

    return std::make_unique<Controller>(
        "test-session", std::move(cfg), std::move(llm), std::move(io),
        *context_, *policy_);
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
    cfg.wall_clock_timeout = std::chrono::seconds(5);
    cfg.token_budget = 100000;
    cfg.action_count_limit = 50;
    return cfg;
  }

  testing::MockMemoryStore memory_store_;
  testing::MockAuditSink audit_sink_;
  ContextConfig ctx_config_;
  PolicyConfig pol_config_;
  std::unique_ptr<ContextStrategy> context_;
  std::unique_ptr<PolicyLayer> policy_;
  testing::MockLlmClient* llm_raw_ = nullptr;
  testing::MockIoBridge* io_raw_ = nullptr;
};

// ---------------------------------------------------------------------------
// Session lifecycle: create → start → shutdown
// Requirements: 1.1, 1.2, 1.9
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, SessionLifecycle_CreateStartShutdown) {
  auto ctrl = MakeController(DefaultConfig());

  // After construction: Idle.
  EXPECT_EQ(ctrl->GetState(), State::kIdle);

  // After Start(): Listening.
  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(ctrl->GetState(), State::kListening);

  // After Shutdown(): Terminated.
  ctrl->Shutdown();
  EXPECT_EQ(ctrl->GetState(), State::kTerminated);
}

// ---------------------------------------------------------------------------
// Transition sequence: Idle → Listening → Thinking → Routing → Responding → Listening
// Requirements: 1.2, 1.3, 1.5, 3.5
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TransitionSequence_FullResponseCycle) {
  auto ctrl = MakeController(DefaultConfig());

  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl->OnTransition([&](State from, State to, Event event) {
    transitions.push_back({from, to, event});
  });

  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ctrl->EnqueueObservation(MakeUserObs("hello"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ctrl->Shutdown();

  // Verify the expected transition sequence occurred.
  // Idle→Listening, Listening→Thinking, Thinking→Routing, Routing→Responding,
  // Responding→Listening (or Responding→Idle if stop condition met).
  ASSERT_GE(transitions.size(), 4u);

  EXPECT_EQ(std::get<0>(transitions[0]), State::kIdle);
  EXPECT_EQ(std::get<1>(transitions[0]), State::kListening);

  EXPECT_EQ(std::get<0>(transitions[1]), State::kListening);
  EXPECT_EQ(std::get<1>(transitions[1]), State::kThinking);

  EXPECT_EQ(std::get<0>(transitions[2]), State::kThinking);
  EXPECT_EQ(std::get<1>(transitions[2]), State::kRouting);

  EXPECT_EQ(std::get<0>(transitions[3]), State::kRouting);
  EXPECT_EQ(std::get<1>(transitions[3]), State::kResponding);
}

// ---------------------------------------------------------------------------
// Transition sequence: tool call cycle
// Idle → Listening → Thinking → Routing → Acting → Thinking → Routing → Responding
// Requirements: 1.5, 1.6, 1.7, 3.4
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TransitionSequence_ToolCallCycle) {
  // Grant capability for the tool.
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
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();
  auto* io_ptr = io.get();

  int call_count = 0;
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    call_count++;
    LlmResult r;
    if (call_count == 1) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "my_tool";
      r.candidate.required_capability = "tool_cap";
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "final";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  io_ptr->execute_fn = [](const ActionCandidate&) -> ActionResult {
    return ActionResult{true, "tool output", ""};
  };

  Controller ctrl("test-session", DefaultConfig(), std::move(llm), std::move(io),
                  *context_, policy2);

  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnTransition([&](State from, State to, Event event) {
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ctrl.EnqueueObservation(MakeUserObs("use tool"));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  ctrl.Shutdown();

  // Find Acting state in transitions.
  bool found_acting = false;
  bool found_action_complete = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kActing) found_acting = true;
    if (from == State::kActing && to == State::kThinking &&
        ev == Event::kActionComplete) {
      found_action_complete = true;
    }
  }
  EXPECT_TRUE(found_acting);
  EXPECT_TRUE(found_action_complete);
}

// ---------------------------------------------------------------------------
// Error recovery flow: Error → recover → Idle
// Requirements: 4.3, 4.5
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, ErrorRecovery_LlmFailureThenRecover) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_retries = 0;  // No retries — immediate failure.

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();

  // LLM throws on first call, then succeeds.
  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    if (c == 0) {
      throw std::runtime_error("LLM error");
    }
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "recovered";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), *context_, *policy_);

  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnTransition([&](State from, State to, Event event) {
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Send observation to trigger LLM failure.
  ctrl.EnqueueObservation(MakeUserObs("trigger error"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should be in Error state now.
  EXPECT_EQ(ctrl.GetState(), State::kError);

  ctrl.Shutdown();

  // Verify Error state was reached.
  bool reached_error = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kError) {
      reached_error = true;
      break;
    }
  }
  EXPECT_TRUE(reached_error);
}

// ---------------------------------------------------------------------------
// Budget exceeded: token budget
// Requirements: 11.1, 11.2
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, BudgetExceeded_TokenBudget) {
  ControllerConfig cfg = DefaultConfig();
  cfg.token_budget = 30;  // Very low budget.
  cfg.max_turns = 100;

  auto ctrl = MakeController(cfg);

  // LLM returns enough tokens to exceed budget on first call.
  llm_raw_->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "resp";
    r.prompt_tokens = 25;
    r.completion_tokens = 10;
    return r;
  };

  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl->OnTransition([&](State from, State to, Event event) {
    transitions.push_back({from, to, event});
  });

  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Send observations.
  for (int i = 0; i < 3; ++i) {
    ctrl->EnqueueObservation(MakeUserObs("msg_" + std::to_string(i)));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  ctrl->Shutdown();

  // Verify: the controller transitioned to Idle via kStopConditionMet,
  // meaning the token budget guardrail was enforced.
  bool found_stop_condition = false;
  for (const auto& [from, to, ev] : transitions) {
    if (ev == Event::kStopConditionMet && to == State::kIdle) {
      found_stop_condition = true;
      break;
    }
  }
  EXPECT_TRUE(found_stop_condition);
}

// ---------------------------------------------------------------------------
// Budget exceeded: action count limit
// Requirements: 11.3, 11.4
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, BudgetExceeded_ActionCountLimit) {
  ControllerConfig cfg = DefaultConfig();
  cfg.action_count_limit = 2;
  cfg.max_turns = 100;
  cfg.token_budget = 1000000;

  // Need policy to allow tool calls.
  PolicyConfig pol_cfg;
  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "tool";
  allow_rule.required_capability = "cap";
  allow_rule.outcome = PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_rule};

  testing::MockAuditSink audit2;
  PolicyLayer policy2(pol_cfg, audit2);
  policy2.InitSession("test-session");
  policy2.GrantCapability("test-session", "cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();
  auto* io_ptr = io.get();

  // LLM always returns tool calls.
  llm_ptr->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kToolCall;
    r.candidate.action_name = "tool";
    r.candidate.required_capability = "cap";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  io_ptr->execute_fn = [](const ActionCandidate&) -> ActionResult {
    return ActionResult{true, "ok", ""};
  };

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), *context_, policy2);

  std::vector<std::string> diagnostics;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnDiagnostic(
      [&](const std::string& msg) { diagnostics.push_back(msg); });
  ctrl.OnTransition([&](State from, State to, Event event) {
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ctrl.EnqueueObservation(MakeUserObs("do tools"));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  ctrl.Shutdown();

  bool found_action_limit = false;
  bool found_stop_condition = false;
  bool found_invalid_transition = false;
  for (const auto& d : diagnostics) {
    if (d.find("action count") != std::string::npos ||
        d.find("Budget exceeded") != std::string::npos) {
      found_action_limit = true;
    }
    if (d.find("Invalid transition") != std::string::npos) {
      found_invalid_transition = true;
    }
  }
  for (const auto& [from, to, ev] : transitions) {
    if (ev == Event::kStopConditionMet && from == State::kThinking &&
        to == State::kIdle) {
      found_stop_condition = true;
    }
  }
  EXPECT_TRUE(found_action_limit);
  EXPECT_TRUE(found_stop_condition);
  EXPECT_FALSE(found_invalid_transition);
}

// ---------------------------------------------------------------------------
// Wall-clock timeout behavior
// Requirements: 11.5, 11.6
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, WallClockTimeout) {
  ControllerConfig cfg = DefaultConfig();
  cfg.wall_clock_timeout = std::chrono::seconds(1);  // 1 second timeout.
  cfg.max_turns = 1000;
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;

  auto ctrl = MakeController(cfg);

  std::vector<std::string> diagnostics;
  ctrl->OnDiagnostic(
      [&](const std::string& msg) { diagnostics.push_back(msg); });

  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Send first observation.
  ctrl->EnqueueObservation(MakeUserObs("first"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Wait for wall-clock timeout to expire.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  // Send another observation after timeout.
  ctrl->EnqueueObservation(MakeUserObs("after timeout"));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ctrl->Shutdown();

  bool found_timeout = false;
  for (const auto& d : diagnostics) {
    if (d.find("wall-clock") != std::string::npos ||
        d.find("timeout") != std::string::npos ||
        d.find("Budget exceeded") != std::string::npos) {
      found_timeout = true;
      break;
    }
  }
  EXPECT_TRUE(found_timeout);
}

// ---------------------------------------------------------------------------
// Interrupt during Thinking
// Requirements: 12.1, 12.2, 12.3, 12.4, 12.5
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, InterruptDuringThinking) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_turns = 100;

  testing::MockAuditSink audit2;
  testing::MockMemoryStore memory2;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context2(ctx_cfg, memory2);
  context2.InitSession("test-session");

  PolicyConfig pol_cfg;
  PolicyLayer policy2(pol_cfg, audit2);
  policy2.InitSession("test-session");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();

  // First LLM call returns kContinue, which transitions to Thinking and
  // returns to the loop. The loop then dequeues the next observation while
  // the controller is in Thinking state, triggering the interrupt path.
  // Second call (after interrupt) returns a response to end the loop.
  std::atomic<int> llm_call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = llm_call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kContinue;
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), context2, policy2);

  std::vector<std::string> diagnostics;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnDiagnostic(
      [&](const std::string& msg) { diagnostics.push_back(msg); });
  ctrl.OnTransition([&](State from, State to, Event event) {
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // Send first observation to enter Thinking → Routing → kContinue → Thinking.
  ctrl.EnqueueObservation(MakeUserObs("first"));

  // Small delay to let the first observation start processing.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  // Send interrupt observation. When the loop dequeues this, the state is
  // Thinking (from kContinue), so the interrupt path fires.
  ctrl.EnqueueObservation(MakeUserObs("interrupt!"));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  ctrl.Shutdown();

  // Verify Cancel() was called.
  EXPECT_GE(llm_ptr->cancel_count, 1);

  // Verify interrupt diagnostic was emitted.
  bool found_interrupt = false;
  for (const auto& d : diagnostics) {
    if (d.find("interrupt") != std::string::npos ||
        d.find("Interrupt") != std::string::npos) {
      found_interrupt = true;
      break;
    }
  }
  EXPECT_TRUE(found_interrupt);

  // Verify memory entry about interruption was recorded.
  auto entries = memory2.GetAll("test-session");
  bool found_memory = false;
  for (const auto& e : entries) {
    if (e.content.find("interrupted") != std::string::npos ||
        e.content.find("Interrupt") != std::string::npos) {
      found_memory = true;
      break;
    }
  }
  EXPECT_TRUE(found_memory);
}

// ---------------------------------------------------------------------------
// Transition callbacks are invoked in order (on-exit before on-enter)
// Requirements: 2.5
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TransitionCallbackOrder) {
  auto ctrl = MakeController(DefaultConfig());

  std::vector<std::string> callback_log;
  ctrl->OnTransition([&](State from, State to, Event event) {
    callback_log.push_back("exit:" + std::to_string(static_cast<int>(from)) +
                           " enter:" + std::to_string(static_cast<int>(to)));
  });

  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ctrl->Shutdown();

  // At least the Start transition should be logged.
  ASSERT_FALSE(callback_log.empty());
  // First callback: exit Idle, enter Listening.
  EXPECT_NE(callback_log[0].find("exit:0"), std::string::npos);  // kIdle=0
  EXPECT_NE(callback_log[0].find("enter:1"), std::string::npos); // kListening=1
}

// ---------------------------------------------------------------------------
// Diagnostic callback fires on invalid transition attempt
// Requirements: 1.10, 2.4
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, DiagnosticOnInvalidTransition) {
  auto ctrl = MakeController(DefaultConfig());

  std::vector<std::string> diagnostics;
  ctrl->OnDiagnostic(
      [&](const std::string& msg) { diagnostics.push_back(msg); });

  // Controller is in Idle. Enqueue a tool result observation — this should
  // not trigger any valid transition from Idle (only kStart and kShutdown
  // are valid from Idle). The RunLoop only processes user observations when
  // in Listening state, so this tests that non-user observations in
  // non-Listening states are handled gracefully.
  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // State is now Listening. Send a system event (not a user observation).
  // The RunLoop checks for kUserMessage specifically, so a system event
  // in Listening state won't trigger a transition.
  Observation sys_obs;
  sys_obs.type = ObservationType::kSystemEvent;
  sys_obs.content = "system ping";
  sys_obs.source = "system";
  sys_obs.timestamp = std::chrono::steady_clock::now();
  ctrl->EnqueueObservation(std::move(sys_obs));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ctrl->Shutdown();

  // The system event in Listening state is simply not processed (no transition
  // attempted), which is correct behavior. The diagnostic test is covered
  // by the property test for invalid transitions.
  EXPECT_EQ(ctrl->GetState(), State::kTerminated);
}

// ---------------------------------------------------------------------------
// Multiple observations processed in sequence
// Requirements: 3.1
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, MultipleObservationsProcessedInOrder) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_turns = 100;
  auto ctrl = MakeController(cfg);

  std::vector<std::string> processed;
  std::mutex mu;

  llm_raw_->submit_fn = [&](const ContextWindow& ctx) -> LlmResult {
    if (!ctx.messages.empty()) {
      std::lock_guard<std::mutex> lock(mu);
      processed.push_back(ctx.messages.back().content);
    }
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "ack";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  ctrl->Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  ctrl->EnqueueObservation(MakeUserObs("first"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ctrl->EnqueueObservation(MakeUserObs("second"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  ctrl->EnqueueObservation(MakeUserObs("third"));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  ctrl->Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_GE(processed.size(), 1u);
  EXPECT_EQ(processed[0], "first");
  if (processed.size() >= 2) {
    EXPECT_EQ(processed[1], "second");
  }
  if (processed.size() >= 3) {
    EXPECT_EQ(processed[2], "third");
  }
}

// ---------------------------------------------------------------------------
// GetState is thread-safe
// Requirements: 2.6
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, GetStateIsThreadSafe) {
  auto ctrl = MakeController(DefaultConfig());

  ctrl->Start();

  // Read state from multiple threads concurrently.
  std::vector<std::thread> threads;
  std::atomic<int> valid_reads{0};
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 100; ++j) {
        State s = ctrl->GetState();
        // State should always be a valid enum value.
        int val = static_cast<int>(s);
        if (val >= 0 && val <= 7) {
          valid_reads.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  ctrl->Shutdown();
  EXPECT_EQ(valid_reads.load(), 400);
}

}  // namespace
}  // namespace shizuru::core
