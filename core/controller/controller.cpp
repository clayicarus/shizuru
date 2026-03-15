#include "controller/controller.h"

#include <chrono>
#include <exception>
#include <string>
#include <thread>

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
Controller::Controller(ControllerConfig config,
                       std::unique_ptr<LlmClient> llm,
                       std::unique_ptr<IoBridge> io,
                       ContextStrategy& context,
                       PolicyLayer& policy)
    : config_(std::move(config)),
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

  // Audit the transition.
  policy_.AuditTransition("default", old_state, new_state, event);

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
  // Check budget first.
  if (CheckBudget()) {
    TryTransition(Event::kStopConditionMet);
    return;
  }

  // Build context window.
  auto window = context_.BuildContext("default", obs);

  // Submit to LLM with retry and exponential backoff.
  LlmResult result;
  bool success = false;
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    try {
      result = llm_->Submit(window);
      success = true;
      break;
    } catch (...) {
      if (attempt == config_.max_retries) {
        TryTransition(Event::kLlmFailure);
        return;
      }
      // Exponential backoff: base_delay * 2^attempt.
      auto delay = config_.retry_base_delay * (1 << attempt);
      std::this_thread::sleep_for(delay);
    }
  }

  if (!success) return;

  // Update token counts.
  total_prompt_tokens_ += result.prompt_tokens;
  total_completion_tokens_ += result.completion_tokens;

  // Increment turn count.
  turn_count_++;

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
      auto permission = policy_.CheckPermission("default", ac);
      if (permission.outcome == PolicyOutcome::kAllow) {
        TryTransition(Event::kRouteToAction);
        HandleActing(std::move(ac));
      } else {
        // Denied — record denial as observation and re-enter thinking.
        MemoryEntry denial_entry;
        denial_entry.type = MemoryEntryType::kToolResult;
        denial_entry.role = "system";
        denial_entry.content =
            "Action denied: " + permission.reason;
        denial_entry.timestamp = std::chrono::steady_clock::now();
        context_.RecordTurn("default", denial_entry);

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
      TryTransition(Event::kRouteToResponse);
      HandleResponding(std::move(ac));
      break;
    case ActionType::kContinue:
      TryTransition(Event::kRouteToContinue);
      // Will re-enter thinking on next loop iteration.
      break;
  }
}

// Execute IO action, record result, transition back to Thinking.
void Controller::HandleActing(ActionCandidate ac) {
  action_count_++;

  auto result = io_->Execute(ac);

  // Record result as MemoryEntry.
  MemoryEntry result_entry;
  result_entry.type = MemoryEntryType::kToolResult;
  result_entry.role = "tool";
  result_entry.content = result.success ? result.output : result.error_message;
  result_entry.tool_call_id = ac.action_name;
  result_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn("default", result_entry);

  if (result.success) {
    TryTransition(Event::kActionComplete);
  } else {
    TryTransition(Event::kActionFailed);
  }

  // Create observation from result and continue thinking.
  Observation result_obs;
  result_obs.type = ObservationType::kToolResult;
  result_obs.content = result.success ? result.output : result.error_message;
  result_obs.source = "tool:" + ac.action_name;
  result_obs.timestamp = std::chrono::steady_clock::now();
  HandleThinking(result_obs);
}

// Deliver response, check stop conditions.
void Controller::HandleResponding(ActionCandidate ac) {
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
  context_.RecordTurn("default", response_entry);

  // Check stop conditions.
  if (turn_count_ >= config_.max_turns ||
      total_prompt_tokens_ + total_completion_tokens_ >= config_.token_budget ||
      action_count_ >= config_.action_count_limit ||
      std::chrono::steady_clock::now() - session_start_ >=
          config_.wall_clock_timeout) {
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
  llm_->Cancel();
  io_->Cancel();

  // Record partial results as MemoryEntry.
  MemoryEntry interrupt_entry;
  interrupt_entry.type = MemoryEntryType::kAssistantMessage;
  interrupt_entry.role = "system";
  interrupt_entry.content = "Turn interrupted";
  interrupt_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn("default", interrupt_entry);

  TryTransition(Event::kInterrupt);  // → Listening

  EmitDiagnostic("Turn interrupted in state " +
                 std::to_string(static_cast<int>(state_.load())));
}

}  // namespace shizuru::core
