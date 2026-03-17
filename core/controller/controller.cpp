#include "controller/controller.h"

#include <chrono>
#include <exception>
#include <string>
#include <thread>

#include "async_logger.h"

namespace shizuru::core {

// Static transition table — all 24 transitions from the design.
const std::unordered_map<std::pair<State, Event>, State, PairHash>
    Controller::kTransitionTable = {
        // Idle
        {{State::kIdle, Event::kStart}, State::kListening},
        {{State::kIdle, Event::kShutdown}, State::kTerminated},

        // Listening
        {{State::kListening, Event::kUserObservation}, State::kThinking},
        {{State::kListening, Event::kShutdown}, State::kTerminated},
        {{State::kListening, Event::kStop}, State::kIdle},

        // Thinking
        {{State::kThinking, Event::kLlmResult}, State::kRouting},
        {{State::kThinking, Event::kLlmFailure}, State::kError},
        {{State::kThinking, Event::kInterrupt}, State::kListening},
        {{State::kThinking, Event::kStopConditionMet}, State::kIdle},
        {{State::kThinking, Event::kShutdown}, State::kTerminated},

        // Routing
        {{State::kRouting, Event::kRouteToAction}, State::kActing},
        {{State::kRouting, Event::kRouteToResponse}, State::kResponding},
        {{State::kRouting, Event::kRouteToContinue}, State::kThinking},
        {{State::kRouting, Event::kInterrupt}, State::kListening},
        {{State::kRouting, Event::kShutdown}, State::kTerminated},

        // Acting
        {{State::kActing, Event::kActionComplete}, State::kThinking},
        {{State::kActing, Event::kActionFailed}, State::kThinking},
        {{State::kActing, Event::kInterrupt}, State::kListening},
        {{State::kActing, Event::kShutdown}, State::kTerminated},

        // Responding
        {{State::kResponding, Event::kResponseDelivered}, State::kListening},
        {{State::kResponding, Event::kStopConditionMet}, State::kIdle},
        {{State::kResponding, Event::kShutdown}, State::kTerminated},

        // Error
        {{State::kError, Event::kRecover}, State::kIdle},
        {{State::kError, Event::kShutdown}, State::kTerminated},
};

// Constructor
Controller::Controller(std::string session_id,
                       ControllerConfig config,
                       std::unique_ptr<LlmClient> llm,
                       std::unique_ptr<IoBridge> io,
                       ContextStrategy& context,
                       PolicyLayer& policy)
    : session_id_(std::move(session_id)),
      config_(std::move(config)),
      llm_(std::move(llm)),
      io_(std::move(io)),
      context_(context),
      policy_(policy) {}

Controller::~Controller() {
  if (loop_thread_.joinable()) {
    Shutdown();
  }
}

// Thread-safe: enqueue an observation from any thread.
void Controller::EnqueueObservation(Observation obs) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    observation_queue_.push_back(std::move(obs));
  }
  queue_cv_.notify_one();
}

// Start the reasoning loop on its own thread.
void Controller::Start() {
  session_start_ = std::chrono::steady_clock::now();
  TryTransition(Event::kStart);
  loop_thread_ = std::thread(&Controller::RunLoop, this);
}

// Request shutdown. Blocks until loop exits.
void Controller::Shutdown() {
  shutdown_requested_.store(true);
  queue_cv_.notify_one();
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
  TryTransition(Event::kShutdown);
}

// Thread-safe state accessor.
State Controller::GetState() const {
  return state_.load();
}

// Register callbacks for state transitions.
void Controller::OnTransition(TransitionCallback cb) {
  transition_callbacks_.push_back(std::move(cb));
}

// Register callback for diagnostic events.
void Controller::OnDiagnostic(DiagnosticCallback cb) {
  diagnostic_callbacks_.push_back(std::move(cb));
}

// Register callback for assistant text responses.
void Controller::OnResponse(ResponseCallback cb) {
  response_callbacks_.push_back(std::move(cb));
}

// Validate + execute transition.
bool Controller::TryTransition(Event event) {
  State current = state_.load();
  auto it = kTransitionTable.find({current, event});
  if (it == kTransitionTable.end()) {
    LOG_WARN("[{}] Invalid transition: {} --[{}]--> ?",
             MODULE_NAME, StateName(current), EventName(event));
    EmitDiagnostic("Invalid transition from state " +
                   std::to_string(static_cast<int>(current)) + " on event " +
                   std::to_string(static_cast<int>(event)));
    return false;
  }

  State old_state = current;
  State new_state = it->second;
  state_.store(new_state);

  // Fire on-exit callbacks for old_state, then on-enter callbacks for new_state.
  for (const auto& cb : transition_callbacks_) {
    cb(old_state, new_state, event);
  }

  // Audit the transition — LogAuditSink handles the debug log output.
  policy_.AuditTransition(session_id_, old_state, new_state, event);

  return true;
}

// Emit diagnostic message to all registered callbacks.
void Controller::EmitDiagnostic(const std::string& message) {
  for (const auto& cb : diagnostic_callbacks_) {
    cb(message);
  }
}

// Main reasoning loop — runs on loop_thread_.
void Controller::RunLoop() {
  while (!shutdown_requested_.load()) {
    Observation obs;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [&] {
        return !observation_queue_.empty() || shutdown_requested_.load();
      });
      if (shutdown_requested_.load()) break;
      obs = std::move(observation_queue_.front());
      observation_queue_.pop_front();
    }

    // Check for interrupt: if we're in Thinking/Routing/Acting and get a user
    // observation, handle the interrupt and re-enqueue the observation.
    State current = state_.load();
    if (obs.type == ObservationType::kUserMessage &&
        (current == State::kThinking || current == State::kRouting ||
         current == State::kActing)) {
      HandleInterrupt();
      // Re-enqueue the observation to process after transitioning to Listening.
      EnqueueObservation(std::move(obs));
      continue;
    }

    // Normal flow: if in Listening and got user observation.
    if (current == State::kListening &&
        obs.type == ObservationType::kUserMessage) {
      LOG_INFO("[{}] User message received: \"{}\"", MODULE_NAME, obs.content);
      TryTransition(Event::kUserObservation);
      // Drive the thinking→routing→acting/responding cycle.
      try {
        HandleThinking(obs);
      } catch (const std::exception& e) {
        EmitDiagnostic("Unhandled exception: " + std::string(e.what()));
        TryTransition(Event::kLlmFailure);
      }
    }
  }
}

// Build context, submit to LLM with retry, route the result.
void Controller::HandleThinking(const Observation& obs) {
  // Record user messages in memory immediately so every subsequent LLM call
  // in this turn (including after tool denial) sees the original question.
  // Re-enter via kContinuation so BuildContext does not duplicate it.
  if (obs.type == ObservationType::kUserMessage) {
    MemoryEntry user_entry;
    user_entry.type = MemoryEntryType::kUserMessage;
    user_entry.role = "user";
    user_entry.content = obs.content;
    user_entry.timestamp = obs.timestamp;
    context_.RecordTurn(session_id_, user_entry);

    Observation cont;
    cont.type = ObservationType::kContinuation;
    cont.source = obs.source;
    cont.timestamp = obs.timestamp;
    HandleThinking(cont);
    return;
  }

  // Check budget first.
  if (CheckBudget()) {
    TryTransition(Event::kStopConditionMet);
    return;
  }

  // Build context window.
  auto window = context_.BuildContext(session_id_, obs);
  LOG_DEBUG("[{}] Context built: {} messages, ~{} tokens",
            MODULE_NAME, window.messages.size(), window.estimated_tokens);

  // Submit to LLM with retry and exponential backoff.
  LlmResult result;
  bool success = false;
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    try {
      LOG_DEBUG("[{}] LLM submit (attempt {}/{})",
                MODULE_NAME, attempt + 1, config_.max_retries + 1);
      result = llm_->Submit(window);
      success = true;
      break;
    } catch (...) {
      if (attempt == config_.max_retries) {
        LOG_ERROR("[{}] LLM submit failed after {} attempts",
                  MODULE_NAME, config_.max_retries + 1);
        TryTransition(Event::kLlmFailure);
        return;
      }
      // Exponential backoff: base_delay * 2^attempt.
      auto delay = config_.retry_base_delay * (1 << attempt);
      LOG_WARN("[{}] LLM submit error, retrying in {}ms",
               MODULE_NAME, std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());
      std::this_thread::sleep_for(delay);
    }
  }

  if (!success) return;

  // Update token counts.
  total_prompt_tokens_ += result.prompt_tokens;
  total_completion_tokens_ += result.completion_tokens;

  // Increment turn count.
  turn_count_++;

  LOG_INFO("[{}] LLM result: turn={}, prompt_tokens={}, completion_tokens={}, total_tokens={}",
           MODULE_NAME, turn_count_, result.prompt_tokens, result.completion_tokens,
           total_prompt_tokens_ + total_completion_tokens_);

  // Transition to Routing.
  TryTransition(Event::kLlmResult);

  // Route the action candidate.
  HandleRouting(std::move(result.candidate));
}

// Route LLM output based on ActionType.
void Controller::HandleRouting(ActionCandidate ac) {
  switch (ac.type) {
    case ActionType::kToolCall: {
      // Check policy permission.
      LOG_INFO("[{}] Routing: tool_call=\"{}\" args={}",
               MODULE_NAME, ac.action_name, ac.arguments);
      auto permission = policy_.CheckPermission(session_id_, ac);
      if (permission.outcome == PolicyOutcome::kAllow) {
        LOG_DEBUG("[{}] Policy: ALLOW tool=\"{}\"", MODULE_NAME, ac.action_name);
        TryTransition(Event::kRouteToAction);
        HandleActing(std::move(ac));
      } else {
        LOG_WARN("[{}] Policy: DENY tool=\"{}\" reason=\"{}\"",
                 MODULE_NAME, ac.action_name, permission.reason);
        // Denied — record denial as observation and re-enter thinking.
        MemoryEntry denial_entry;
        denial_entry.type = MemoryEntryType::kToolResult;
        denial_entry.role = "system";
        denial_entry.content =
            "Action denied: " + permission.reason;
        denial_entry.timestamp = std::chrono::steady_clock::now();
        context_.RecordTurn(session_id_, denial_entry);

        TryTransition(Event::kRouteToContinue);

        // Create observation from denial and re-enter thinking.
        Observation denial_obs;
        denial_obs.type = ObservationType::kSystemEvent;
        denial_obs.content = "Action denied: " + permission.reason;
        denial_obs.source = "policy";
        denial_obs.timestamp = std::chrono::steady_clock::now();
        HandleThinking(denial_obs);
      }
      break;
    }
    case ActionType::kResponse:
      LOG_INFO("[{}] Routing: response text_len={}",
               MODULE_NAME, ac.response_text.size());
      TryTransition(Event::kRouteToResponse);
      HandleResponding(std::move(ac));
      break;
    case ActionType::kContinue:
      LOG_DEBUG("[{}] Routing: continue (no action, no response)", MODULE_NAME);
      TryTransition(Event::kRouteToContinue);
      // Will re-enter thinking on next loop iteration.
      break;
  }
}

// Execute IO action, record result, transition back to Thinking.
void Controller::HandleActing(ActionCandidate ac) {
  action_count_++;
  LOG_INFO("[{}] Acting: tool=\"{}\" call_id=\"{}\" args={}",
           MODULE_NAME, ac.action_name, ac.response_text, ac.arguments);

  // Record the assistant's tool call decision.
  // ac.response_text holds the LLM-assigned tool_call_id (set in json_parser).
  // ac.arguments is already a serialized JSON string from the LLM.
  const std::string& tc_id =
      ac.response_text.empty() ? ac.action_name : ac.response_text;
  std::string tc_json =
      "[{\"id\":\"" + tc_id + "\",\"type\":\"function\","
      "\"function\":{\"name\":\"" + ac.action_name + "\","
      "\"arguments\":" + ac.arguments + "}}]";

  MemoryEntry call_entry;
  call_entry.type = MemoryEntryType::kToolCall;
  call_entry.role = "assistant";
  call_entry.content = "";
  call_entry.tool_call_id = tc_id;
  call_entry.tool_calls_json = tc_json;
  call_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, call_entry);

  auto result = io_->Execute(ac);

  if (result.success) {
    LOG_INFO("[{}] Tool result: tool=\"{}\" output={}",
             MODULE_NAME, ac.action_name, result.output);
  } else {
    LOG_WARN("[{}] Tool failed: tool=\"{}\" error=\"{}\"",
             MODULE_NAME, ac.action_name, result.error_message);
  }

  // Record the tool result. Use ac.response_text as the tool_call_id so OpenAI
  // can pair the tool result with the assistant's tool call above.
  MemoryEntry result_entry;
  result_entry.type = MemoryEntryType::kToolResult;
  result_entry.role = "tool";
  result_entry.content = result.success ? result.output : result.error_message;
  result_entry.tool_call_id = ac.response_text.empty() ? ac.action_name : ac.response_text;
  result_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, result_entry);

  if (result.success) {
    TryTransition(Event::kActionComplete);
  } else {
    TryTransition(Event::kActionFailed);
  }

  // Use kContinuation so BuildContext does not re-append the tool result as a
  // duplicate current observation (it is already in memory above).
  Observation continuation;
  continuation.type = ObservationType::kContinuation;
  continuation.source = "controller";
  continuation.timestamp = std::chrono::steady_clock::now();
  HandleThinking(continuation);
}

// Deliver response, check stop conditions.
void Controller::HandleResponding(ActionCandidate ac) {
  LOG_INFO("[{}] Responding: \"{}\"", MODULE_NAME, ac.response_text);

  // Notify response callbacks before final state transition.
  for (const auto& cb : response_callbacks_) {
    cb(ac);
  }

  // Record response as MemoryEntry.
  MemoryEntry response_entry;
  response_entry.type = MemoryEntryType::kAssistantMessage;
  response_entry.role = "assistant";
  response_entry.content = ac.response_text;
  response_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, response_entry);

  // Check stop conditions.
  if (turn_count_ >= config_.max_turns ||
      total_prompt_tokens_ + total_completion_tokens_ >= config_.token_budget ||
      action_count_ >= config_.action_count_limit ||
      std::chrono::steady_clock::now() - session_start_ >=
          config_.wall_clock_timeout) {
    LOG_INFO("[{}] Stop condition met: turns={}, tokens={}, actions={}",
             MODULE_NAME, turn_count_,
             total_prompt_tokens_ + total_completion_tokens_,
             action_count_);
    TryTransition(Event::kStopConditionMet);  // → Idle
  } else {
    TryTransition(Event::kResponseDelivered);  // → Listening
  }
}

// Enforce budget guardrails. Returns true if any limit is exceeded.
bool Controller::CheckBudget() {
  if (turn_count_ >= config_.max_turns) {
    EmitDiagnostic("Budget exceeded: max turns (" +
                   std::to_string(config_.max_turns) + ")");
    return true;
  }
  if (total_prompt_tokens_ + total_completion_tokens_ >=
      config_.token_budget) {
    EmitDiagnostic("Budget exceeded: token budget (" +
                   std::to_string(config_.token_budget) + ")");
    return true;
  }
  if (action_count_ >= config_.action_count_limit) {
    EmitDiagnostic("Budget exceeded: action count limit (" +
                   std::to_string(config_.action_count_limit) + ")");
    return true;
  }
  if (std::chrono::steady_clock::now() - session_start_ >=
      config_.wall_clock_timeout) {
    EmitDiagnostic("Budget exceeded: wall-clock timeout");
    return true;
  }
  return false;
}

// Cancel in-progress work and transition to Listening.
void Controller::HandleInterrupt() {
  LOG_WARN("[{}] Interrupt received in state {}", MODULE_NAME, StateName(state_.load()));
  llm_->Cancel();
  io_->Cancel();

  // Record partial results as MemoryEntry.
  MemoryEntry interrupt_entry;
  interrupt_entry.type = MemoryEntryType::kAssistantMessage;
  interrupt_entry.role = "system";
  interrupt_entry.content = "Turn interrupted";
  interrupt_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, interrupt_entry);

  TryTransition(Event::kInterrupt);  // → Listening

  EmitDiagnostic("Turn interrupted in state " +
                 std::to_string(static_cast<int>(state_.load())));
}

}  // namespace shizuru::core
