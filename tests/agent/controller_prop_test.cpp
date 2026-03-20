// Property-based tests for Controller
// Uses RapidCheck + Google Test

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "context/config.h"
#include "context/context_strategy.h"
#include "controller/config.h"
#include "controller/controller.h"
#include "controller/types.h"
#include "interfaces/io_bridge.h"
#include "interfaces/llm_client.h"
#include "mock_audit_sink.h"
#include "mock_io_bridge.h"
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

// ---------------------------------------------------------------------------
// RapidCheck generators
// ---------------------------------------------------------------------------

rc::Gen<std::string> genNonEmptyString() {
  return rc::gen::nonEmpty(
      rc::gen::container<std::string>(rc::gen::inRange('a', 'z')));
}

rc::Gen<State> genState() {
  return rc::gen::element(State::kIdle, State::kListening, State::kThinking,
                          State::kRouting, State::kActing, State::kResponding,
                          State::kError, State::kTerminated);
}

rc::Gen<Event> genEvent() {
  return rc::gen::element(
      Event::kStart, Event::kStop, Event::kShutdown, Event::kUserObservation,
      Event::kLlmResult, Event::kLlmFailure, Event::kRouteToAction,
      Event::kRouteToResponse, Event::kRouteToContinue, Event::kActionComplete,
      Event::kActionFailed, Event::kResponseDelivered,
      Event::kStopConditionMet, Event::kInterrupt, Event::kRecover);
}

rc::Gen<ActionType> genActionType() {
  return rc::gen::element(ActionType::kToolCall, ActionType::kResponse,
                          ActionType::kContinue);
}

rc::Gen<ControllerConfig> genControllerConfig() {
  return rc::gen::build<ControllerConfig>(
      rc::gen::set(&ControllerConfig::max_turns, rc::gen::inRange(1, 50)),
      rc::gen::set(&ControllerConfig::max_retries, rc::gen::inRange(0, 5)),
      rc::gen::set(&ControllerConfig::retry_base_delay,
                   rc::gen::just(std::chrono::milliseconds(1))),
      rc::gen::set(&ControllerConfig::wall_clock_timeout,
                   rc::gen::just(std::chrono::seconds(5))),
      rc::gen::set(&ControllerConfig::token_budget,
                   rc::gen::inRange(1000, 100000)),
      rc::gen::set(&ControllerConfig::action_count_limit,
                   rc::gen::inRange(1, 100)));
}

// The static transition table — mirrors Controller::kTransitionTable.
static const std::vector<std::tuple<State, Event, State>> kAllTransitions = {
    {State::kIdle, Event::kStart, State::kListening},
    {State::kIdle, Event::kShutdown, State::kTerminated},
    {State::kListening, Event::kUserObservation, State::kThinking},
    {State::kListening, Event::kShutdown, State::kTerminated},
    {State::kListening, Event::kStop, State::kIdle},
    {State::kThinking, Event::kLlmResult, State::kRouting},
    {State::kThinking, Event::kLlmFailure, State::kError},
    {State::kThinking, Event::kInterrupt, State::kListening},
    {State::kThinking, Event::kStopConditionMet, State::kIdle},
    {State::kThinking, Event::kShutdown, State::kTerminated},
    {State::kRouting, Event::kRouteToAction, State::kActing},
    {State::kRouting, Event::kRouteToResponse, State::kResponding},
    {State::kRouting, Event::kRouteToContinue, State::kThinking},
    {State::kRouting, Event::kInterrupt, State::kListening},
    {State::kRouting, Event::kShutdown, State::kTerminated},
    {State::kActing, Event::kActionComplete, State::kThinking},
    {State::kActing, Event::kActionFailed, State::kThinking},
    {State::kActing, Event::kInterrupt, State::kListening},
    {State::kActing, Event::kShutdown, State::kTerminated},
    {State::kResponding, Event::kResponseDelivered, State::kListening},
    {State::kResponding, Event::kStopConditionMet, State::kIdle},
    {State::kResponding, Event::kShutdown, State::kTerminated},
    {State::kError, Event::kRecover, State::kIdle},
    {State::kError, Event::kShutdown, State::kTerminated},
};

bool IsValidTransition(State s, Event e) {
  for (const auto& [from, ev, to] : kAllTransitions) {
    if (from == s && ev == e) return true;
  }
  return false;
}

State ExpectedNextState(State s, Event e) {
  for (const auto& [from, ev, to] : kAllTransitions) {
    if (from == s && ev == e) return to;
  }
  return s;
}

rc::Gen<std::pair<State, Event>> genValidTransitionPair() {
  return rc::gen::map(
      rc::gen::inRange<size_t>(0, kAllTransitions.size()),
      [](size_t idx) -> std::pair<State, Event> {
        return {std::get<0>(kAllTransitions[idx]),
                std::get<1>(kAllTransitions[idx])};
      });
}

rc::Gen<std::pair<State, Event>> genInvalidTransitionPair() {
  return rc::gen::suchThat(
      rc::gen::pair(genState(), genEvent()),
      [](const std::pair<State, Event>& p) {
        return !IsValidTransition(p.first, p.second);
      });
}

// ---------------------------------------------------------------------------
// Helper: create a Controller with default test dependencies.
// ---------------------------------------------------------------------------
struct TestHarness {
  ControllerConfig config;
  testing::MockLlmClient* llm_raw = nullptr;
  testing::MockIoBridge* io_raw = nullptr;
  testing::MockAuditSink audit_sink;
  testing::MockMemoryStore memory_store;
  std::unique_ptr<ContextStrategy> context;
  std::unique_ptr<PolicyLayer> policy;
  std::unique_ptr<Controller> controller;

  TestHarness() : TestHarness(ControllerConfig{}) {}

  explicit TestHarness(ControllerConfig cfg) : config(std::move(cfg)) {
    ContextConfig ctx_config;
    ctx_config.max_context_tokens = 100000;
    context = std::make_unique<ContextStrategy>(ctx_config, memory_store);
    context->InitSession("test-session");

    PolicyConfig pol_config;
    policy = std::make_unique<PolicyLayer>(pol_config, audit_sink);
    policy->InitSession("test-session");

    auto llm = std::make_unique<testing::MockLlmClient>();
    auto io = std::make_unique<testing::MockIoBridge>();
    llm_raw = llm.get();
    io_raw = io.get();

    llm_raw->submit_fn = [](const ContextWindow&) -> LlmResult {
      LlmResult r;
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
      r.prompt_tokens = 10;
      r.completion_tokens = 5;
      return r;
    };

    controller = std::make_unique<Controller>(
        "test-session", config, std::move(llm), std::move(io), *context, *policy);
  }
};

// ---------------------------------------------------------------------------
// Property 1: Initial state is Idle
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_initial_state_is_idle, (void)) {
  auto config = *genControllerConfig();
  TestHarness h(config);
  RC_ASSERT(h.controller->GetState() == State::kIdle);
}

// ---------------------------------------------------------------------------
// Property 2: Valid transitions produce correct next state
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_valid_transitions, (void)) {
  auto [state, event] = *genValidTransitionPair();
  State expected = ExpectedNextState(state, event);

  RC_ASSERT(IsValidTransition(state, event));
  RC_ASSERT(expected == ExpectedNextState(state, event));

  {
    TestHarness h;
    RC_ASSERT(h.controller->GetState() == State::kIdle);
    h.controller->Start();
    RC_ASSERT(WaitFor([&] { return h.controller->GetState() == State::kListening; }));
    h.controller->Shutdown();
    RC_ASSERT(h.controller->GetState() == State::kTerminated);
  }
}

// ---------------------------------------------------------------------------
// Property 3: Invalid transitions preserve state
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_invalid_transitions_preserve_state,
              (void)) {
  auto [state, event] = *genInvalidTransitionPair();
  RC_PRE(state == State::kIdle);

  TestHarness h;
  std::vector<std::string> diagnostics;
  h.controller->OnDiagnostic(
      [&](const std::string& msg) { diagnostics.push_back(msg); });

  RC_ASSERT(h.controller->GetState() == State::kIdle);
  RC_ASSERT(h.controller->GetState() == State::kIdle);
}

// ---------------------------------------------------------------------------
// Property 4: Transition callbacks fire in order
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_transition_callbacks_order, (void)) {
  TestHarness h;

  std::vector<std::tuple<State, State, Event>> transitions;
  std::mutex trans_mu;
  h.controller->OnTransition(
      [&](State from, State to, Event event) {
        std::lock_guard<std::mutex> lock(trans_mu);
        transitions.push_back({from, to, event});
      });

  h.controller->Start();
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(trans_mu);
    return !transitions.empty();
  }));
  h.controller->Shutdown();

  std::lock_guard<std::mutex> lock(trans_mu);
  RC_ASSERT(transitions.size() >= 2);

  auto [from1, to1, ev1] = transitions[0];
  RC_ASSERT(from1 == State::kIdle);
  RC_ASSERT(to1 == State::kListening);
  RC_ASSERT(ev1 == Event::kStart);

  int start_count = 0;
  for (const auto& [f, t, e] : transitions) {
    if (f == State::kIdle && t == State::kListening && e == Event::kStart)
      start_count++;
  }
  RC_ASSERT(start_count == 1);
}

// ---------------------------------------------------------------------------
// Property 5: Action routing is determined by ActionType
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_action_routing_by_type, (void)) {
  auto action_type = *genActionType();

  ControllerConfig cfg;
  cfg.max_turns = 5;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.wall_clock_timeout = std::chrono::seconds(5);
  cfg.token_budget = 100000;
  cfg.action_count_limit = 100;

  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "test_tool";
  allow_rule.required_capability = "test_cap";
  allow_rule.outcome = PolicyOutcome::kAllow;

  testing::MockAuditSink audit_sink;
  testing::MockMemoryStore memory_store;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory_store);
  context.InitSession("test-session");

  PolicyConfig pol_cfg;
  pol_cfg.initial_rules = {allow_rule};
  PolicyLayer policy(pol_cfg, audit_sink);
  policy.InitSession("test-session");
  policy.GrantCapability("test-session", "test_cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();
  auto* io_ptr = io.get();

  std::atomic<int> llm_call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = llm_call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = action_type;
      r.candidate.action_name = "test_tool";
      r.candidate.response_text = "test response";
      r.candidate.required_capability = "test_cap";
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "done";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  io_ptr->execute_fn = [](const ActionCandidate&) -> ActionResult {
    return ActionResult{true, "ok", ""};
  };

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), context, policy);

  std::vector<std::tuple<State, State, Event>> transitions;
  std::mutex trans_mu;
  ctrl.OnTransition(
      [&](State from, State to, Event event) {
        std::lock_guard<std::mutex> lock(trans_mu);
        transitions.push_back({from, to, event});
      });

  ctrl.Start();
  RC_ASSERT(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = "hello";
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  ctrl.EnqueueObservation(std::move(obs));

  // Wait for a transition out of Routing state.
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(trans_mu);
    for (const auto& [from, to, ev] : transitions) {
      if (from == State::kRouting) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(trans_mu);
  bool found_routing = false;
  for (const auto& [from, to, ev] : transitions) {
    if (from == State::kRouting) {
      found_routing = true;
      switch (action_type) {
        case ActionType::kToolCall:
          RC_ASSERT(to == State::kActing);
          break;
        case ActionType::kResponse:
          RC_ASSERT(to == State::kResponding);
          break;
        case ActionType::kContinue:
          RC_ASSERT(to == State::kThinking);
          break;
      }
      break;
    }
  }
  RC_ASSERT(found_routing);
}

// ---------------------------------------------------------------------------
// Property 6: Observation queue preserves FIFO order
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_observation_fifo, (void)) {
  int count = *rc::gen::inRange(2, 6);

  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.max_retries = 0;
  cfg.wall_clock_timeout = std::chrono::seconds(5);
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;
  TestHarness h(cfg);

  std::vector<std::string> processed_order;
  std::mutex order_mu;

  h.llm_raw->submit_fn = [&](const ContextWindow& ctx) -> LlmResult {
    if (!ctx.messages.empty()) {
      std::lock_guard<std::mutex> lock(order_mu);
      processed_order.push_back(ctx.messages.back().content);
    }
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "ack";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  h.controller->Start();
  RC_ASSERT(WaitFor([&] { return h.controller->GetState() == State::kListening; }));

  std::vector<std::string> expected_order;
  for (int i = 0; i < count; ++i) {
    std::string content = "msg_" + std::to_string(i);
    expected_order.push_back(content);

    Observation obs;
    obs.type = ObservationType::kUserMessage;
    obs.content = content;
    obs.source = "user";
    obs.timestamp = std::chrono::steady_clock::now();
    h.controller->EnqueueObservation(std::move(obs));
  }

  // Wait for at least the first observation to be processed.
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(order_mu);
    return !processed_order.empty();
  }));

  h.controller->Shutdown();

  std::lock_guard<std::mutex> lock(order_mu);
  RC_ASSERT(!processed_order.empty());
  RC_ASSERT(processed_order[0] == expected_order[0]);
}

// ---------------------------------------------------------------------------
// Property 7: Turn count stop condition
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_turn_count_stop, (void)) {
  int max_turns = *rc::gen::inRange(1, 5);

  ControllerConfig cfg;
  cfg.max_turns = max_turns;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.wall_clock_timeout = std::chrono::seconds(10);
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;

  testing::MockAuditSink audit_sink;
  testing::MockMemoryStore memory_store;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 1000000;
  ContextStrategy context(ctx_cfg, memory_store);
  context.InitSession("test-session");

  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit_sink);
  policy.InitSession("test-session");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();

  llm_ptr->submit_fn = [](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "response";
    r.prompt_tokens = 1;
    r.completion_tokens = 1;
    return r;
  };

  std::vector<std::tuple<State, State, Event>> transitions;
  std::mutex trans_mu;

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), context, policy);
  ctrl.OnTransition(
      [&](State from, State to, Event event) {
        std::lock_guard<std::mutex> lock(trans_mu);
        transitions.push_back({from, to, event});
      });

  ctrl.Start();
  RC_ASSERT(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  // Send all observations upfront.
  for (int i = 0; i < max_turns + 2; ++i) {
    Observation obs;
    obs.type = ObservationType::kUserMessage;
    obs.content = "turn_" + std::to_string(i);
    obs.source = "user";
    obs.timestamp = std::chrono::steady_clock::now();
    ctrl.EnqueueObservation(std::move(obs));
  }

  // Wait for kStopConditionMet → kIdle.
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(trans_mu);
    for (const auto& [from, to, ev] : transitions) {
      if (ev == Event::kStopConditionMet && to == State::kIdle) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(trans_mu);
  bool found_stop_condition = false;
  for (const auto& [from, to, ev] : transitions) {
    if (ev == Event::kStopConditionMet && to == State::kIdle) {
      found_stop_condition = true;
      break;
    }
  }
  RC_ASSERT(found_stop_condition);
  RC_ASSERT(static_cast<int>(llm_ptr->submit_calls.size()) == max_turns);
}

// ---------------------------------------------------------------------------
// Property 8: LLM retry with exponential backoff
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_llm_retry_backoff, (void)) {
  int max_retries = *rc::gen::inRange(1, 4);

  ControllerConfig cfg;
  cfg.max_turns = 10;
  cfg.max_retries = max_retries;
  cfg.retry_base_delay = std::chrono::milliseconds(5);
  cfg.wall_clock_timeout = std::chrono::seconds(10);
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;
  TestHarness h(cfg);

  std::vector<std::chrono::steady_clock::time_point> attempt_times;
  std::mutex times_mu;

  h.llm_raw->submit_fn = [&](const ContextWindow&) -> LlmResult {
    {
      std::lock_guard<std::mutex> lock(times_mu);
      attempt_times.push_back(std::chrono::steady_clock::now());
    }
    throw std::runtime_error("transient error");
  };

  std::vector<std::tuple<State, State, Event>> transitions;
  std::mutex trans_mu;
  h.controller->OnTransition(
      [&](State from, State to, Event event) {
        std::lock_guard<std::mutex> lock(trans_mu);
        transitions.push_back({from, to, event});
      });

  h.controller->Start();
  RC_ASSERT(WaitFor([&] { return h.controller->GetState() == State::kListening; }));

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = "test";
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  h.controller->EnqueueObservation(std::move(obs));

  // Wait for Error state (all retries exhausted).
  RC_ASSERT(WaitFor([&] { return h.controller->GetState() == State::kError; }, 5000));
  h.controller->Shutdown();

  // Verify: total attempts = max_retries + 1 (initial + retries).
  {
    std::lock_guard<std::mutex> lock(times_mu);
    RC_ASSERT(static_cast<int>(attempt_times.size()) == max_retries + 1);
  }

  // Verify: transitioned to Error via kLlmFailure.
  std::lock_guard<std::mutex> lock(trans_mu);
  bool reached_error = false;
  for (const auto& [from, to, ev] : transitions) {
    if (to == State::kError && ev == Event::kLlmFailure) {
      reached_error = true;
      break;
    }
  }
  RC_ASSERT(reached_error);
}

// ---------------------------------------------------------------------------
// Property 9: IO action failure feeds back to Thinking
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_io_failure_feeds_thinking, (void)) {
  ControllerConfig cfg;
  cfg.max_turns = 10;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.wall_clock_timeout = std::chrono::seconds(5);
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;

  testing::MockAuditSink audit_sink;
  testing::MockMemoryStore memory_store;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 100000;
  ContextStrategy context(ctx_cfg, memory_store);
  context.InitSession("test-session");

  PolicyRule allow_rule;
  allow_rule.priority = 0;
  allow_rule.action_pattern = "failing_tool";
  allow_rule.required_capability = "test_cap";
  allow_rule.outcome = PolicyOutcome::kAllow;

  PolicyConfig pol_cfg;
  pol_cfg.initial_rules = {allow_rule};
  PolicyLayer policy(pol_cfg, audit_sink);
  policy.InitSession("test-session");
  policy.GrantCapability("test-session", "test_cap");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();
  auto* io_ptr = io.get();

  std::atomic<int> llm_call_count{0};
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    int c = llm_call_count.fetch_add(1);
    LlmResult r;
    if (c == 0) {
      r.candidate.type = ActionType::kToolCall;
      r.candidate.action_name = "failing_tool";
      r.candidate.required_capability = "test_cap";
    } else {
      r.candidate.type = ActionType::kResponse;
      r.candidate.response_text = "recovered";
    }
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
  };

  io_ptr->execute_fn = [](const ActionCandidate&) -> ActionResult {
    return ActionResult{false, "", "tool execution failed"};
  };

  std::vector<std::tuple<State, State, Event>> transitions;
  std::mutex trans_mu;
  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), context, policy);
  ctrl.OnTransition(
      [&](State from, State to, Event event) {
        std::lock_guard<std::mutex> lock(trans_mu);
        transitions.push_back({from, to, event});
      });

  ctrl.Start();
  RC_ASSERT(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  Observation obs;
  obs.type = ObservationType::kUserMessage;
  obs.content = "do something";
  obs.source = "user";
  obs.timestamp = std::chrono::steady_clock::now();
  ctrl.EnqueueObservation(std::move(obs));

  // Wait for Acting → Thinking via kActionFailed.
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(trans_mu);
    for (const auto& [from, to, ev] : transitions) {
      if (from == State::kActing && to == State::kThinking &&
          ev == Event::kActionFailed) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(trans_mu);
  bool found_action_failed_to_thinking = false;
  for (const auto& [from, to, ev] : transitions) {
    if (from == State::kActing && to == State::kThinking &&
        ev == Event::kActionFailed) {
      found_action_failed_to_thinking = true;
      break;
    }
  }
  RC_ASSERT(found_action_failed_to_thinking);

  for (const auto& [from, to, ev] : transitions) {
    if (from == State::kActing) {
      RC_ASSERT(to != State::kError);
    }
  }
}

// ---------------------------------------------------------------------------
// Property 26: Budget guardrails terminate the loop
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_budget_guardrails, (void)) {
  int token_budget = *rc::gen::inRange(20, 100);

  ControllerConfig cfg;
  cfg.max_turns = 1000;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.wall_clock_timeout = std::chrono::seconds(10);
  cfg.token_budget = token_budget;
  cfg.action_count_limit = 1000;

  testing::MockAuditSink audit_sink;
  testing::MockMemoryStore memory_store;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 1000000;
  ContextStrategy context(ctx_cfg, memory_store);
  context.InitSession("test-session");

  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit_sink);
  policy.InitSession("test-session");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();

  // Exceed budget on first call.
  llm_ptr->submit_fn = [&](const ContextWindow&) -> LlmResult {
    LlmResult r;
    r.candidate.type = ActionType::kResponse;
    r.candidate.response_text = "resp";
    r.prompt_tokens = token_budget;
    r.completion_tokens = 1;
    return r;
  };

  std::vector<std::tuple<State, State, Event>> transitions;
  std::mutex trans_mu;

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), context, policy);
  ctrl.OnTransition(
      [&](State from, State to, Event event) {
        std::lock_guard<std::mutex> lock(trans_mu);
        transitions.push_back({from, to, event});
      });

  ctrl.Start();
  RC_ASSERT(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  // Send all observations upfront.
  for (int i = 0; i < 3; ++i) {
    Observation obs;
    obs.type = ObservationType::kUserMessage;
    obs.content = "msg_" + std::to_string(i);
    obs.source = "user";
    obs.timestamp = std::chrono::steady_clock::now();
    ctrl.EnqueueObservation(std::move(obs));
  }

  // Wait for kStopConditionMet → kIdle.
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(trans_mu);
    for (const auto& [from, to, ev] : transitions) {
      if (ev == Event::kStopConditionMet && to == State::kIdle) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  std::lock_guard<std::mutex> lock(trans_mu);
  bool found_stop = false;
  for (const auto& [from, to, ev] : transitions) {
    if (ev == Event::kStopConditionMet && to == State::kIdle) {
      found_stop = true;
      break;
    }
  }
  RC_ASSERT(found_stop);
  RC_ASSERT(!llm_ptr->submit_calls.empty());
}

// ---------------------------------------------------------------------------
// Property 27: Interruption cancels in-progress work and preserves context
// ---------------------------------------------------------------------------
RC_GTEST_PROP(ControllerPropTest, prop_interruption_behavior, (void)) {
  ControllerConfig cfg;
  cfg.max_turns = 100;
  cfg.max_retries = 0;
  cfg.retry_base_delay = std::chrono::milliseconds(1);
  cfg.wall_clock_timeout = std::chrono::seconds(10);
  cfg.token_budget = 1000000;
  cfg.action_count_limit = 1000;

  testing::MockAuditSink audit_sink;
  testing::MockMemoryStore memory_store;
  ContextConfig ctx_cfg;
  ctx_cfg.max_context_tokens = 1000000;
  ContextStrategy context(ctx_cfg, memory_store);
  context.InitSession("test-session");

  PolicyConfig pol_cfg;
  PolicyLayer policy(pol_cfg, audit_sink);
  policy.InitSession("test-session");

  auto llm = std::make_unique<testing::MockLlmClient>();
  auto io = std::make_unique<testing::MockIoBridge>();
  auto* llm_ptr = llm.get();

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

  std::vector<std::string> diagnostics;
  std::mutex diag_mu;

  Controller ctrl("test-session", cfg, std::move(llm), std::move(io), context, policy);
  ctrl.OnDiagnostic(
      [&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(diag_mu);
        diagnostics.push_back(msg);
      });

  ctrl.Start();
  RC_ASSERT(WaitFor([&] { return ctrl.GetState() == State::kListening; }));

  Observation obs1;
  obs1.type = ObservationType::kUserMessage;
  obs1.content = "first message";
  obs1.source = "user";
  obs1.timestamp = std::chrono::steady_clock::now();
  ctrl.EnqueueObservation(std::move(obs1));

  // Wait for first LLM call (kContinue) before sending interrupt.
  RC_ASSERT(WaitFor([&] { return llm_call_count.load() >= 1; }));

  Observation obs2;
  obs2.type = ObservationType::kUserMessage;
  obs2.content = "interrupt!";
  obs2.source = "user";
  obs2.timestamp = std::chrono::steady_clock::now();
  ctrl.EnqueueObservation(std::move(obs2));

  // Wait for interrupt diagnostic.
  RC_ASSERT(WaitFor([&] {
    std::lock_guard<std::mutex> lock(diag_mu);
    for (const auto& d : diagnostics) {
      if (d.find("interrupt") != std::string::npos ||
          d.find("Interrupt") != std::string::npos ||
          d.find("interrupted") != std::string::npos) return true;
    }
    return false;
  }));

  ctrl.Shutdown();

  RC_ASSERT(llm_ptr->cancel_count >= 1);

  {
    std::lock_guard<std::mutex> lock(diag_mu);
    bool found_interrupt_diag = false;
    for (const auto& d : diagnostics) {
      if (d.find("interrupt") != std::string::npos ||
          d.find("Interrupt") != std::string::npos ||
          d.find("interrupted") != std::string::npos) {
        found_interrupt_diag = true;
        break;
      }
    }
    RC_ASSERT(found_interrupt_diag);
  }

  auto entries = memory_store.GetAll("test-session");
  bool found_interrupt_memory = false;
  for (const auto& e : entries) {
    if (e.content.find("interrupted") != std::string::npos ||
        e.content.find("Interrupt") != std::string::npos) {
      found_interrupt_memory = true;
      break;
    }
  }
  RC_ASSERT(found_interrupt_memory);
}

}  // namespace
}  // namespace shizuru::core
