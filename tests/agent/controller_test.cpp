// Unit tests for Controller
// Tests specific examples, edge cases, and integration scenarios.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
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
#include "policy/types.h"

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

  std::unique_ptr<Controller> MakeController(ControllerConfig cfg) {
    auto llm = std::make_unique<testing::MockLlmClient>();
    llm_raw_ = llm.get();

    llm_raw_->submit_fn = [](const ContextWindow&) -> LlmResult {
      LlmResult r;
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    };

    return std::make_unique<Controller>(
        "test-session", std::move(cfg), std::move(llm),
        nullptr, nullptr,
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
    cfg.turn_timeout = std::chrono::seconds(5);
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
};

// ---------------------------------------------------------------------------
// Session lifecycle: create → start → shutdown
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, SessionLifecycle_CreateStartShutdown) {
  auto ctrl = MakeController(DefaultConfig());

  EXPECT_EQ(ctrl->GetState(), State::kIdle);

  ctrl->Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl->GetState() == State::kListening; }));

  ctrl->Shutdown();
  EXPECT_EQ(ctrl->GetState(), State::kTerminated);
}

// ---------------------------------------------------------------------------
// Transition sequence: full response cycle
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TransitionSequence_FullResponseCycle) {
  auto ctrl = MakeController(DefaultConfig());

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl->OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl->Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl->GetState() == State::kListening; }));

  ctrl->EnqueueObservation(MakeUserObs("hello"));

  // Wait until we've seen at least 4 transitions (Idle→Listen→Think→Route→Respond).
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return transitions.size() >= 4;
  }));

  ctrl->Shutdown();

  std::lock_guard<std::mutex> lock(mu);
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
// Transition sequence: tool call cycle (async via EmitFrameCallback + kToolResult)
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TransitionSequence_ToolCallCycle) {
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
      {
        ToolCall tc;
        tc.id = "call_test_1";
        tc.name = "my_tool";
        tc.arguments = "{}";
        tc.required_capability = "tool_cap";
        r.candidate.tool_calls.push_back(std::move(tc));
      }
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "final";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // Capture the controller pointer so we can enqueue kToolResult from the callback.
  Controller* ctrl_ptr = nullptr;
  std::mutex ctrl_mu;

  // EmitFrameCallback: when action_out fires, enqueue a kToolResult observation.
  Controller::EmitFrameCallback emit_frame = [&](const std::string& port,
                                                  io::DataFrame /*frame*/) {
    if (port == "action_out") {
      std::lock_guard<std::mutex> lock(ctrl_mu);
      if (ctrl_ptr) {
        Observation result_obs;
        result_obs.type = ObservationType::kToolResult;
        result_obs.content = R"({"success":true,"output":"tool output"})";
        result_obs.source = "tool:my_tool";
        result_obs.timestamp = std::chrono::steady_clock::now();
        ctrl_ptr->EnqueueObservation(std::move(result_obs));
      }
    }
  };

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  std::move(emit_frame), nullptr,
                  *context_, policy2);

  {
    std::lock_guard<std::mutex> lock(ctrl_mu);
    ctrl_ptr = &ctrl;
  }

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("use tool"));

  // Wait until Acting→Thinking (kActionComplete) transition appears.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [from, to, ev] : transitions) {
      if (from == State::kActing && to == State::kThinking &&
          ev == Event::kActionComplete) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  bool found_acting = false;
  bool found_action_complete = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kActing) found_acting = true;
    if (from == State::kActing && to == State::kThinking &&
        ev == Event::kActionComplete) found_action_complete = true;
  }
  EXPECT_TRUE(found_acting);
  EXPECT_TRUE(found_action_complete);
}

// ---------------------------------------------------------------------------
// Error recovery: LLM failure → Error state
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, ErrorRecovery_LlmFailureThenRecover) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_retries = 0;

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    if (c == 0) throw std::runtime_error("LLM error");
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "recovered";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm), nullptr, nullptr, *context_, *policy_);

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

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  bool reached_error = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kError) { reached_error = true; break; }
  }
  EXPECT_TRUE(reached_error);
}

// ---------------------------------------------------------------------------
// Budget exceeded: token budget
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, BudgetExceeded_TokenBudget) {
  ControllerConfig cfg = DefaultConfig();
  cfg.token_budget = 30;
  cfg.max_turns = 100;

  auto ctrl = MakeController(cfg);

  llm_raw_->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "resp";
    r.prompt_tokens = 25;
    r.completion_tokens = 10;
    return r;
  };

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl->OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl->Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl->GetState() == State::kListening; }));

  // Send 3 observations sequentially, waiting for each to be processed.
  for (int i = 0; i < 3; ++i) {
    ctrl->EnqueueObservation(MakeUserObs("msg_" + std::to_string(i)));
    // Wait until controller returns to Idle (budget hit) or Listening (still going).
    WaitFor([&] {
      auto s = ctrl->GetState();
      return s == State::kListening || s == State::kIdle;
    });
  }

  // Wait for kStopConditionMet to appear.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [from, to, ev] : transitions) {
      if (ev == Event::kStopConditionMet && to == State::kIdle) return true;
    }
    return false;
  }));

  ctrl->Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  bool found = false;
  for (const auto& [from, to, ev] : transitions) {
    if (ev == Event::kStopConditionMet && to == State::kIdle) { found = true; break; }
  }
  EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Budget exceeded: action count limit
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, BudgetExceeded_ActionCountLimit) {
  ControllerConfig cfg = DefaultConfig();
  cfg.action_count_limit = 2;
  cfg.max_turns = 100;
  cfg.token_budget = 1000000;

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
  auto* llm_ptr = llm.get();

  // Capture controller pointer to enqueue kToolResult from emit callback.
  Controller* ctrl_ptr2 = nullptr;
  std::mutex ctrl_mu2;

  Controller::EmitFrameCallback emit_frame2 = [&](const std::string& port,
                                                    io::DataFrame /*frame*/) {
    if (port == "action_out") {
      std::lock_guard<std::mutex> lock(ctrl_mu2);
      if (ctrl_ptr2) {
        Observation result_obs;
        result_obs.type = ObservationType::kToolResult;
        result_obs.content = R"({"success":true,"output":"ok"})";
        result_obs.source = "tool";
        result_obs.timestamp = std::chrono::steady_clock::now();
        ctrl_ptr2->EnqueueObservation(std::move(result_obs));
      }
    }
  };

  llm_ptr->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kToolCall;
    r.candidate.action_name = "tool";
    r.candidate.required_capability = "cap";
    {
      ToolCall tc;
      tc.id = "call_budget_1";
      tc.name = "tool";
      tc.arguments = "{}";
      tc.required_capability = "cap";
      r.candidate.tool_calls.push_back(std::move(tc));
    }
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm),
                  std::move(emit_frame2), nullptr,
                  *context_, policy2);

  {
    std::lock_guard<std::mutex> lock(ctrl_mu2);
    ctrl_ptr2 = &ctrl;
  }

  std::mutex mu;
  std::vector<std::string> diagnostics;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl.OnDiagnostic([&](const std::string& msg) {
    std::lock_guard<std::mutex> lock(mu);
    diagnostics.push_back(msg);
  });
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("do tools"));

  // Wait for kStopConditionMet.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [from, to, ev] : transitions) {
      if (ev == Event::kStopConditionMet && from == State::kThinking &&
          to == State::kIdle) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  bool found_action_limit = false;
  bool found_stop_condition = false;
  bool found_invalid_transition = false;
  for (const auto& d : diagnostics) {
    if (d.find("action count") != std::string::npos ||
        d.find("Budget exceeded") != std::string::npos)
      found_action_limit = true;
    if (d.find("Invalid transition") != std::string::npos)
      found_invalid_transition = true;
  }
  for (const auto& [from, to, ev] : transitions) {
    if (ev == Event::kStopConditionMet && from == State::kThinking &&
        to == State::kIdle)
      found_stop_condition = true;
  }
  EXPECT_TRUE(found_action_limit);
  EXPECT_TRUE(found_stop_condition);
  EXPECT_FALSE(found_invalid_transition);
}

// ---------------------------------------------------------------------------
// Turn timeout — LLM sleeps past the timeout so HandleResponding fires it
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TurnTimeout) {
  ControllerConfig cfg = DefaultConfig();
  cfg.turn_timeout = std::chrono::seconds(1);
  cfg.max_turns = 1000;
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;

  auto ctrl = MakeController(cfg);

  // LLM sleeps 1.1s — longer than turn_timeout — so the turn-timeout
  // check in HandleResponding fires after the first response is delivered.
  llm_raw_->submit_fn = [](const ContextWindow&) -> LlmResult {
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "ok";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  ctrl->OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });

  ctrl->Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl->GetState() == State::kListening; }));

  ctrl->EnqueueObservation(MakeUserObs("first"));

  // Wait up to 2s for kStopConditionMet → kIdle (LLM takes ~1.1s).
  bool found_stop = WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [from, to, ev] : transitions) {
      if (ev == Event::kStopConditionMet && to == State::kIdle) return true;
    }
    return false;
  }, 2000);

  ctrl->Shutdown();
  EXPECT_TRUE(found_stop) << "kStopConditionMet never fired after turn timeout";
}

// ---------------------------------------------------------------------------
// Idle can be reawakened by a new user message after StopConditionMet.
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, IdleStateCanWakeOnNewUserObservation) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_turns = 1;
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;
  cfg.turn_timeout = std::chrono::seconds(30);

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> llm_call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    const int call = llm_call_count.fetch_add(1);
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = call == 0 ? "first" : "second";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm), nullptr, nullptr,
                  *context_, *policy_);

  std::mutex mu;
  std::vector<std::tuple<State, State, Event>> transitions;
  std::vector<std::string> responses;
  ctrl.OnTransition([&](State from, State to, Event event) {
    std::lock_guard<std::mutex> lock(mu);
    transitions.push_back({from, to, event});
  });
  ctrl.OnResponse([&](const ActionCandidate& ac) {
    std::lock_guard<std::mutex> lock(mu);
    responses.push_back(ac.response_text);
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("first"));
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kIdle; }));

  ctrl.EnqueueObservation(MakeUserObs("second"));
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return responses.size() >= 2;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(responses.size(), 2u);
  EXPECT_EQ(responses[0], "first");
  EXPECT_EQ(responses[1], "second");

  bool found_idle_restart = false;
  int user_observation_count = 0;
  for (const auto& [from, to, ev] : transitions) {
    if (from == State::kIdle && to == State::kListening &&
        ev == Event::kStart) {
      found_idle_restart = true;
    }
    if (from == State::kListening && to == State::kThinking &&
        ev == Event::kUserObservation) {
      ++user_observation_count;
    }
  }
  EXPECT_TRUE(found_idle_restart);
  EXPECT_GE(user_observation_count, 2);
}

// ---------------------------------------------------------------------------
// Interrupt during Thinking
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
  auto* llm_ptr = llm.get();

  std::atomic<int> llm_call_count{0};
  std::atomic<bool> first_call_entered{false};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = llm_call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      // Signal that we've entered the LLM call, then block until cancelled.
      first_call_entered.store(true);
      llm_ptr->WaitForCancel(2000);
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "cancelled";
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  Controller ctrl("test-session", cfg, std::move(llm), nullptr, nullptr, context2, policy2);

  std::mutex mu;
  std::vector<std::string> diagnostics;
  ctrl.OnDiagnostic([&](const std::string& msg) {
    std::lock_guard<std::mutex> lock(mu);
    diagnostics.push_back(msg);
  });

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("first"));

  // Wait for the first LLM call to start (loop thread is now blocked in submit).
  ASSERT_TRUE(WaitFor([&] { return first_call_entered.load(); }));

  // Call Interrupt() which sets interrupt_requested_ and calls Cancel().
  ctrl.Interrupt();

  // Wait for interrupt diagnostic.
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& d : diagnostics) {
      if (d.find("interrupt") != std::string::npos ||
          d.find("Interrupt") != std::string::npos) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  EXPECT_EQ(llm_call_count.load(), 1);

  bool found_interrupt = false;
  {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& d : diagnostics) {
      if (d.find("interrupt") != std::string::npos ||
          d.find("Interrupt") != std::string::npos) { found_interrupt = true; break; }
    }
  }
  EXPECT_TRUE(found_interrupt);

  auto entries = memory2.GetAll("test-session");
  bool found_memory = false;
  bool found_empty_user_message = false;
  for (const auto& e : entries) {
    if (e.content.find("interrupted") != std::string::npos ||
        e.content.find("Interrupt") != std::string::npos) {
      found_memory = true;
    }
    if (e.role == "user" && e.content.empty()) {
      found_empty_user_message = true;
    }
  }
  EXPECT_TRUE(found_memory);
  EXPECT_FALSE(found_empty_user_message);
}

// ---------------------------------------------------------------------------
// Transition callbacks are invoked in order
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, TransitionCallbackOrder) {
  auto ctrl = MakeController(DefaultConfig());

  std::mutex mu;
  std::vector<std::string> callback_log;
  ctrl->OnTransition([&](State from, State to, Event) {
    std::lock_guard<std::mutex> lock(mu);
    callback_log.push_back("exit:" + std::to_string(static_cast<int>(from)) +
                           " enter:" + std::to_string(static_cast<int>(to)));
  });

  ctrl->Start();
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return !callback_log.empty();
  }));
  ctrl->Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_FALSE(callback_log.empty());
  EXPECT_NE(callback_log[0].find("exit:0"), std::string::npos);
  EXPECT_NE(callback_log[0].find("enter:1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Diagnostic callback fires on invalid transition attempt
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, DiagnosticOnInvalidTransition) {
  auto ctrl = MakeController(DefaultConfig());

  std::vector<std::string> diagnostics;
  ctrl->OnDiagnostic(
      [&](const std::string& msg) { diagnostics.push_back(msg); });

  ctrl->Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl->GetState() == State::kListening; }));

  Observation sys_obs;
  sys_obs.type = ObservationType::kSystemEvent;
  sys_obs.content = "system ping";
  sys_obs.source = "system";
  sys_obs.timestamp = std::chrono::steady_clock::now();
  ctrl->EnqueueObservation(std::move(sys_obs));

  // Brief wait for the observation to be consumed, then shut down.
  WaitFor([&] { return false; }, 50);  // 50ms settle
  ctrl->Shutdown();

  EXPECT_EQ(ctrl->GetState(), State::kTerminated);
}

// ---------------------------------------------------------------------------
// Multiple observations processed in sequence
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, MultipleObservationsProcessedInOrder) {
  ControllerConfig cfg = DefaultConfig();
  cfg.max_turns = 100;
  auto ctrl = MakeController(cfg);

  std::mutex mu;
  std::vector<std::string> processed;

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
  ASSERT_TRUE(WaitFor([&] { return ctrl->GetState() == State::kListening; }));

  // Send each observation and wait for it to be processed before sending the next.
  ctrl->EnqueueObservation(MakeUserObs("first"));
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return processed.size() >= 1;
  }));

  ctrl->EnqueueObservation(MakeUserObs("second"));
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return processed.size() >= 2;
  }));

  ctrl->EnqueueObservation(MakeUserObs("third"));
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return processed.size() >= 3;
  }));

  ctrl->Shutdown();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_GE(processed.size(), 3u);
  EXPECT_EQ(processed[0], "first");
  EXPECT_EQ(processed[1], "second");
  EXPECT_EQ(processed[2], "third");
}

// ---------------------------------------------------------------------------
// GetState is thread-safe
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, GetStateIsThreadSafe) {
  auto ctrl = MakeController(DefaultConfig());

  ctrl->Start();

  std::vector<std::thread> threads;
  std::atomic<int> valid_reads{0};
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < 100; ++j) {
        int val = static_cast<int>(ctrl->GetState());
        if (val >= 0 && val <= 7) valid_reads.fetch_add(1);
      }
    });
  }

  for (auto& t : threads) t.join();

  ctrl->Shutdown();
  EXPECT_EQ(valid_reads.load(), 400);
}

// ---------------------------------------------------------------------------
// HandleInterrupt while kActing invokes CancelCallback
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, HandleInterrupt_WhileActing_InvokesCancelCallback) {
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

  // LLM returns a tool call on first invocation; never returns on second
  // (interrupt will fire before that).
  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "slow_tool";
      r.candidate.required_capability = "tool_cap";
      {
        ToolCall tc;
        tc.id = "call_slow_1";
        tc.name = "slow_tool";
        tc.arguments = "{}";
        tc.required_capability = "tool_cap";
        r.candidate.tool_calls.push_back(std::move(tc));
      }
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // Track CancelCallback invocations.
  std::atomic<int> cancel_invoked{0};
  Controller::CancelCallback cancel_cb = [&]() {
    cancel_invoked.fetch_add(1);
  };

  // EmitFrameCallback: do NOT enqueue kToolResult — leave controller in kActing
  // so we can interrupt it.
  Controller::EmitFrameCallback emit_frame = [](const std::string&,
                                                io::DataFrame) {};

  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  std::move(emit_frame), std::move(cancel_cb),
                  *context_, policy2);

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("use tool"));

  // Wait until controller enters kActing (emit_frame fired, no result enqueued).
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kActing; }));

  // Send an interrupt (user message while kActing triggers HandleInterrupt).
  ctrl.EnqueueObservation(MakeUserObs("interrupt!"));

  // Wait for transition back to kListening.
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.Shutdown();

  EXPECT_GE(cancel_invoked.load(), 1)
      << "CancelCallback must be invoked when interrupt fires during kActing";
}

// ---------------------------------------------------------------------------
// cancel = nullptr + HandleInterrupt does not crash
// ---------------------------------------------------------------------------
TEST_F(ControllerTest, HandleInterrupt_NullCancel_DoesNotCrash) {
  PolicyConfig pol_cfg;
  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "any_tool";
  allow_rule.required_capability = "cap";
  allow_rule.outcome = PolicyOutcome::kAllow;
  pol_cfg.initial_rules = {allow_rule};

  testing::MockAuditSink audit2;
  PolicyLayer policy2(pol_cfg, audit2);
  policy2.InitSession("test-session");
  policy2.GrantCapability("test-session", "cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto* llm_ptr = llm.get();

  std::atomic<int> call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "any_tool";
      r.candidate.required_capability = "cap";
      {
        ToolCall tc;
        tc.id = "call_any_1";
        tc.name = "any_tool";
        tc.arguments = "{}";
        tc.required_capability = "cap";
        r.candidate.tool_calls.push_back(std::move(tc));
      }
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  // EmitFrameCallback: do NOT enqueue kToolResult — leave controller in kActing.
  Controller::EmitFrameCallback emit_frame = [](const std::string&,
                                                io::DataFrame) {};

  // cancel = nullptr — must not crash when HandleInterrupt is called.
  Controller ctrl("test-session", DefaultConfig(), std::move(llm),
                  std::move(emit_frame), nullptr /* cancel */,
                  *context_, policy2);

  ctrl.Start();
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.EnqueueObservation(MakeUserObs("use tool"));

  // Wait until controller enters kActing.
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kActing; }));

  // Send interrupt — must not crash even though cancel_ is nullptr.
  ctrl.EnqueueObservation(MakeUserObs("interrupt!"));

  // Wait for transition back to kListening.
  ASSERT_TRUE(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  ctrl.Shutdown();
  // If we reach here without crashing, the test passes.
  SUCCEED();
}

}  // namespace
}  // namespace shizuru::core
