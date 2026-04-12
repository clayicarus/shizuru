#include "controller/controller.h"

#include <cassert>
#include <chrono>
#include <exception>
#include <string>
#include <thread>

#include "async_logger.h"
#include "io/data_frame.h"

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
                       EmitFrameCallback emit_frame,
                       CancelCallback cancel,
                       ContextStrategy& context,
                       PolicyLayer& policy,
                       std::unique_ptr<ObservationAggregator> observation_aggregator,
                       std::unique_ptr<ObservationFilter> observation_filter,
                       std::unique_ptr<TtsSegmentStrategy> tts_segment,
                       std::unique_ptr<ResponseFilter> response_filter)
    : session_id_(std::move(session_id)),
      config_(std::move(config)),
      llm_(std::move(llm)),
      emit_frame_(std::move(emit_frame)),
      cancel_(std::move(cancel)),
      context_(context),
      policy_(policy),
      observation_aggregator_(observation_aggregator
                                  ? std::move(observation_aggregator)
                                  : std::make_unique<PassthroughAggregator>()),
      observation_filter_(observation_filter
                              ? std::move(observation_filter)
                              : std::make_unique<AcceptAllFilter>()),
      tts_segment_(std::move(tts_segment)),
      response_filter_(response_filter
                           ? std::move(response_filter)
                           : std::make_unique<PassthroughFilter>()) {}

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
  last_activity_ = session_start_;
  conversation_active_ = false;
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
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnTransition must be called before Start()");
  transition_callbacks_.push_back(std::move(cb));
}

// Register callback for diagnostic events.
void Controller::OnDiagnostic(DiagnosticCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnDiagnostic must be called before Start()");
  diagnostic_callbacks_.push_back(std::move(cb));
}

// Register callback for assistant text responses.
void Controller::OnResponse(ResponseCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnResponse must be called before Start()");
  response_callbacks_.push_back(std::move(cb));
}

// Register callback for streaming token deltas.
void Controller::OnStreamToken(StreamTokenCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnStreamToken must be called before Start()");
  stream_token_callbacks_.push_back(std::move(cb));
}

// Register callback for structured activity events.
void Controller::OnActivity(ActivityCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnActivity must be called before Start()");
  activity_callbacks_.push_back(std::move(cb));
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

  if (event == Event::kStopConditionMet && new_state == State::kIdle) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    observation_queue_.clear();
  }

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

// Emit structured activity event to all registered callbacks.
void Controller::EmitActivity(ActivityKind kind, std::string detail) {
  ActivityEvent event{kind, std::move(detail)};
  for (const auto& cb : activity_callbacks_) {
    cb(event);
  }
}

// Main reasoning loop — runs on loop_thread_.
void Controller::RunLoop() {
  while (!shutdown_requested_.load()) {
    Observation obs;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      // Use timed wait when aggregator has pending content or tool calls pending.
      auto wait_duration = observation_aggregator_->HasPending()
                               ? std::chrono::milliseconds(500)
                               : std::chrono::milliseconds(60000);

      if (state_.load() == State::kActing && !pending_tool_calls_.empty()) {
        auto remaining = config_.tool_call_timeout -
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - tool_call_start_);
        if (remaining.count() <= 0) {
          LOG_ERROR("[{}] Tool call timeout ({} pending results)",
                    MODULE_NAME,
                    pending_tool_calls_.size() - pending_results_.size());
          for (const auto& tc : pending_tool_calls_) {
            if (pending_results_.find(tc.id) == pending_results_.end()) {
              pending_results_[tc.id] =
                  R"({"success":false,"error":"tool call timeout"})";
            }
          }
          TryTransition(Event::kActionFailed);
          pending_tool_calls_.clear();
          pending_results_.clear();
          Observation cont;
          cont.type = ObservationType::kContinuation;
          cont.source = "controller";
          cont.timestamp = std::chrono::steady_clock::now();
          lock.unlock();
          HandleThinking(cont);
          continue;
        }
        wait_duration = std::min(wait_duration,
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
      }

      queue_cv_.wait_for(lock, wait_duration, [&] {
        return !observation_queue_.empty() || shutdown_requested_.load();
      });
      if (shutdown_requested_.load()) break;

      // Check aggregator timeout (force-flush buffered content).
      if (observation_queue_.empty() && state_.load() == State::kListening) {
        auto timeout_obs = observation_aggregator_->CheckTimeout();
        if (timeout_obs.has_value()) {
          // Timeout flush — run through filter only, skip aggregator.
          bool accepted = observation_filter_->ShouldProcess(*timeout_obs);
          if (accepted) {
            LOG_INFO("[{}] User message received (timeout): \"{}\"",
                     MODULE_NAME, timeout_obs->content);
            session_start_ = std::chrono::steady_clock::now();
            last_activity_ = session_start_;
            conversation_active_ = true;
            TryTransition(Event::kUserObservation);
            lock.unlock();
            try {
              HandleThinking(*timeout_obs);
            } catch (const std::exception& e) {
              EmitDiagnostic("Unhandled exception: " + std::string(e.what()));
              TryTransition(Event::kLlmFailure);
            }
          } else {
            LOG_INFO("[{}] Timeout content filtered out: \"{}\"",
                     MODULE_NAME, timeout_obs->content);
          }
          continue;
        }
      }

      if (observation_queue_.empty()) continue;
      obs = std::move(observation_queue_.front());
      observation_queue_.pop_front();
    }

    // Internal interrupt event: cancel the in-flight turn without creating
    // a synthetic user message or re-entering the loop with empty content.
    State current = state_.load();
    if (obs.type == ObservationType::kInterruption) {
      if (current == State::kThinking || current == State::kRouting ||
          current == State::kActing) {
        HandleInterrupt();
      }
      continue;
    }

    // Check for barge-in: if we're in Thinking/Routing/Acting and receive a
    // real user observation, interrupt the current turn and reprocess the
    // actual observation after transitioning back to Listening.
    if (obs.type == ObservationType::kUserMessage &&
        (current == State::kThinking || current == State::kRouting ||
         current == State::kActing)) {
      HandleInterrupt();
      // Re-enqueue the observation to process after transitioning to Listening.
      EnqueueObservation(std::move(obs));
      continue;
    }

    // kToolResult branch: resume from kActing when a tool result arrives.
    if (state_.load() == State::kActing &&
        obs.type == ObservationType::kToolResult) {
      HandleActingResult(obs);
      continue;
    }

    // Normal flow: if in Listening/Idle and got user observation.
    if ((current == State::kListening || current == State::kIdle) &&
        obs.type == ObservationType::kUserMessage) {
      const auto now = std::chrono::steady_clock::now();
      const bool reset_window =
          current == State::kIdle ||
          !conversation_active_ ||
          (now - last_activity_ >= config_.conversation_idle_timeout);

      if (reset_window) {
        ResetBudgetWindow();
        conversation_active_ = true;
      }
      last_activity_ = now;

      if (current == State::kIdle) {
        if (!TryTransition(Event::kStart)) {
          continue;
        }
      }

      // Stage 1: Aggregation (endpointing).
      auto aggregated = observation_aggregator_->Feed(obs);
      if (!aggregated.has_value()) {
        LOG_INFO("[{}] Observation buffered by aggregator: \"{}\"",
                 MODULE_NAME, obs.content);
        EmitDiagnostic("Waiting for more input: \"" + obs.content + "\"");
        EmitActivity(ActivityKind::kBufferingInput, obs.content);
        continue;  // Stay in kListening, wait for more fragments.
      }

      // Stage 2: Relevance filter.
      EmitActivity(ActivityKind::kFilteringInput);
      auto filter_start = std::chrono::steady_clock::now();
      bool accepted = observation_filter_->ShouldProcess(*aggregated);
      auto filter_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - filter_start).count();
      LOG_INFO("[{}] ObservationFilter took {}ms", MODULE_NAME, filter_ms);
      if (!accepted) {
        LOG_INFO("[{}] Observation filtered out: \"{}\"",
                 MODULE_NAME, aggregated->content);
        EmitDiagnostic("Filtered out: \"" + aggregated->content + "\"");
        continue;  // Stay in kListening.
      }

      LOG_INFO("[{}] User message received: \"{}\"",
               MODULE_NAME, aggregated->content);
      session_start_ = std::chrono::steady_clock::now();
      last_activity_ = session_start_;
      conversation_active_ = true;
      TryTransition(Event::kUserObservation);
      // Drive the thinking→routing→acting/responding cycle.
      try {
        HandleThinking(*aggregated);
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
    EmitActivity(ActivityKind::kBudgetExhausted);
    return;
  }

  // An interrupt may land after we enter kThinking but before the LLM call
  // starts. Bail out here so the queued interruption can be processed on the
  // next RunLoop iteration without issuing a stale request.
  if (interrupt_requested_.load()) {
    return;
  }

  // Build context window.
  auto window = context_.BuildContext(session_id_, obs);
  first_token_logged_ = false;  // Reset for this turn's latency measurement.
  in_thinking_block_ = false;   // Reset thinking tag state for this turn.
  thinking_tag_buf_.clear();
  LOG_DEBUG("[{}] Context built: {} messages, ~{} tokens",
            MODULE_NAME, window.messages.size(), window.estimated_tokens);

  // Submit to LLM with retry and exponential backoff.
  LlmResult result;
  bool success = false;
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    try {
      LOG_DEBUG("[{}] LLM submit (attempt {}/{})",
                MODULE_NAME, attempt + 1, config_.max_retries + 1);
      LOG_INFO("[{}] LLM submit started", MODULE_NAME);
      if (attempt == 0) {
        EmitActivity(ActivityKind::kThinkingStarted);
      } else {
        EmitActivity(ActivityKind::kThinkingRetry,
                     std::to_string(attempt + 1));
      }
      if (config_.use_streaming) {
        // Streaming path: fire token callbacks as chunks arrive.
        // If a TTS segment strategy is configured, also buffer tokens
        // and emit TTS-ready frames when the strategy signals readiness.
        result = llm_->SubmitStreaming(window, [this](const std::string& token) {
          // Bail early if interrupt was requested.
          if (interrupt_requested_.load()) { return; }
          // Log the first streaming token for latency measurement.
          if (!first_token_logged_) {
            LOG_INFO("[{}] LLM first token received", MODULE_NAME);
            first_token_logged_ = true;
          }
          // Always fire raw token callbacks (for display / UI).
          // UI receives the full stream including <think> tags and decides
          // how to render them.
          for (const auto& cb : stream_token_callbacks_) {
            cb(token);
          }
          // TTS segmentation: filter out structured blocks before feeding to TTS.
          // Strips <think>...</think>, <tool_call>...</tool_call>, and
          // <tool_result>...</tool_result> from the token stream.
          if (tts_segment_) {
            std::string tts_clean;
            for (char ch : token) {
              thinking_tag_buf_ += ch;
              if (in_thinking_block_) {
                // Inside a block — look for closing tags.
                if (thinking_tag_buf_.size() >= 8 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 8) == "</think>") {
                  in_thinking_block_ = false;
                  thinking_tag_buf_.clear();
                } else if (thinking_tag_buf_.size() >= 12 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 12) == "</tool_call>") {
                  in_thinking_block_ = false;
                  thinking_tag_buf_.clear();
                } else if (thinking_tag_buf_.size() >= 14 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 14) == "</tool_result>") {
                  in_thinking_block_ = false;
                  thinking_tag_buf_.clear();
                }
              } else {
                // Outside blocks — look for opening tags.
                bool matched = false;
                // Check <think> (7 chars)
                if (thinking_tag_buf_.size() >= 7 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 7) == "<think>") {
                  in_thinking_block_ = true;
                  if (tts_clean.size() >= 6) tts_clean.erase(tts_clean.size() - 6);
                  else tts_clean.clear();
                  thinking_tag_buf_.clear();
                  matched = true;
                }
                // Check <tool_call> (11 chars)
                if (!matched && thinking_tag_buf_.size() >= 11 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 11) == "<tool_call>") {
                  in_thinking_block_ = true;
                  if (tts_clean.size() >= 10) tts_clean.erase(tts_clean.size() - 10);
                  else tts_clean.clear();
                  thinking_tag_buf_.clear();
                  matched = true;
                }
                // Check <tool_result> (13 chars)
                if (!matched && thinking_tag_buf_.size() >= 13 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 13) == "<tool_result>") {
                  in_thinking_block_ = true;
                  if (tts_clean.size() >= 12) tts_clean.erase(tts_clean.size() - 12);
                  else tts_clean.clear();
                  thinking_tag_buf_.clear();
                  matched = true;
                }
                if (!matched) {
                  tts_clean += ch;
                }
              }
            }
            if (!tts_clean.empty()) {
              tts_segment_->Append(tts_clean);
            }
            size_t ready = tts_segment_->ReadyLength();
            if (ready > 0) {
              // Extract the ready portion and emit as TTS frame.
              // We need to peek at the buffer content before consuming.
              // Flush returns all content, so we use a temporary approach:
              // consume ready chars by flushing and re-appending remainder.
              std::string all = tts_segment_->Flush();
              std::string tts_text = all.substr(0, ready);
              std::string remainder = all.substr(ready);
              if (!remainder.empty()) {
                tts_segment_->Append(remainder);
              }
              if (!tts_text.empty() && emit_frame_) {
                LOG_INFO("[{}] TTS segment ready: \"{}\" (len={})",
                         MODULE_NAME, tts_text, tts_text.size());
                io::DataFrame frame;
                frame.type = "text/plain";
                frame.payload = std::vector<uint8_t>(
                    tts_text.begin(), tts_text.end());
                frame.metadata["tts_ready"] = "1";
                frame.timestamp = std::chrono::steady_clock::now();
                emit_frame_("tts_out", std::move(frame));
              }
            }
          }
        });
        // Streaming complete — flush any remaining TTS buffer.
        if (tts_segment_) {
          std::string remaining = tts_segment_->Flush();
          if (!remaining.empty() && emit_frame_) {
            LOG_INFO("[{}] TTS segment final flush: \"{}\" (len={})",
                     MODULE_NAME, remaining, remaining.size());
            io::DataFrame frame;
            frame.type = "text/plain";
            frame.payload = std::vector<uint8_t>(
                remaining.begin(), remaining.end());
            frame.metadata["tts_ready"] = "1";
            frame.metadata["tts_final"] = "1";
            frame.timestamp = std::chrono::steady_clock::now();
            emit_frame_("tts_out", std::move(frame));
          }
        }
        // Check if interrupt was requested during streaming — don't route partial result.
        if (interrupt_requested_.load()) {
          return;
        }
      } else {
        result = llm_->Submit(window);
        // Check if interrupt was requested during submit — don't route partial result.
        if (interrupt_requested_.load()) {
          return;
        }
      }
      success = true;
      break;
    } catch (...) {
      // If the exception was caused by an interrupt cancellation, bail out
      // immediately — RunLoop will pick up the enqueued interrupt observation.
      if (interrupt_requested_.load()) {
        return;
      }
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
      // Check policy permission for each tool call.
      LOG_INFO("[{}] Routing: {} tool call(s)",
               MODULE_NAME, ac.tool_calls.size());

      bool all_allowed = true;
      std::string denied_reason;
      for (const auto& tc : ac.tool_calls) {
        // Build a temporary ActionCandidate for policy check.
        ActionCandidate single;
        single.type = ActionType::kToolCall;
        single.action_name = tc.name;
        single.arguments = tc.arguments;
        single.required_capability = tc.required_capability;

        auto permission = policy_.CheckPermission(session_id_, single);
        if (permission.outcome != PolicyOutcome::kAllow) {
          LOG_WARN("[{}] Policy: DENY tool=\"{}\" reason=\"{}\"",
                   MODULE_NAME, tc.name, permission.reason);
          all_allowed = false;
          denied_reason = permission.reason;
          break;
        }
        LOG_DEBUG("[{}] Policy: ALLOW tool=\"{}\"", MODULE_NAME, tc.name);
      }

      if (all_allowed) {
        TryTransition(Event::kRouteToAction);
        HandleActing(std::move(ac));
      } else {
        // Denied — record denial as observation and re-enter thinking.
        MemoryEntry denial_entry;
        denial_entry.type = MemoryEntryType::kToolResult;
        denial_entry.role = "system";
        denial_entry.content =
            "Action denied: " + denied_reason;
        denial_entry.timestamp = std::chrono::steady_clock::now();
        context_.RecordTurn(session_id_, denial_entry);

        TryTransition(Event::kRouteToContinue);

        // Create observation from denial and re-enter thinking.
        Observation denial_obs;
        denial_obs.type = ObservationType::kSystemEvent;
        denial_obs.content = "Action denied: " + denied_reason;
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
      {
        Observation continuation;
        continuation.type      = ObservationType::kContinuation;
        continuation.source    = "controller";
        continuation.timestamp = std::chrono::steady_clock::now();
        HandleThinking(continuation);
      }
      break;
  }
}

// Emit action/tool_call frames non-blocking; store pending state for HandleActingResult.
// Supports parallel tool calls: emits one frame per tool call, waits for all results.
void Controller::HandleActing(ActionCandidate ac) {
  pending_action_ = ac;
  pending_tool_calls_ = ac.tool_calls;
  pending_results_.clear();
  tool_call_start_ = std::chrono::steady_clock::now();

  // Build the tool_calls JSON array for memory recording.
  std::string tc_json = "[";
  for (size_t i = 0; i < ac.tool_calls.size(); ++i) {
    const auto& tc = ac.tool_calls[i];
    if (i > 0) tc_json += ",";
    tc_json += R"({"id":")" + tc.id + R"(","type":"function",)";
    tc_json += R"("function":{"name":")" + tc.name + R"(",)";
    tc_json += R"("arguments":)" + tc.arguments + R"(}})";
  }
  tc_json += "]";

  // Record the assistant's tool call decision in memory (single entry for all calls).
  MemoryEntry call_entry;
  call_entry.type = MemoryEntryType::kToolCall;
  call_entry.role = "assistant";
  call_entry.content = "";
  call_entry.tool_call_id = ac.tool_calls.empty() ? "" : ac.tool_calls[0].id;
  call_entry.tool_calls_json = tc_json;
  call_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, call_entry);

  // Emit one action frame per tool call.
  for (const auto& tc : ac.tool_calls) {
    action_count_++;
    LOG_INFO("[{}] Acting: tool=\"{}\" id=\"{}\" args={}",
             MODULE_NAME, tc.name, tc.id, tc.arguments);
    EmitDiagnostic("Tool call: " + tc.name);
    {
      // Build a JSON detail string for the UI to render a tool call card.
      std::string detail = R"({"id":")" + tc.id +
                           R"(","name":")" + tc.name +
                           R"(","arguments":)" + tc.arguments + "}";
      EmitActivity(ActivityKind::kToolDispatched, std::move(detail));
    }

    const std::string payload_str = tc.name + ":" + tc.arguments;
    io::DataFrame frame;
    frame.type = "action/tool_call";
    frame.payload = std::vector<uint8_t>(payload_str.begin(), payload_str.end());
    frame.metadata["tool_call_id"] = tc.id;
    frame.timestamp = std::chrono::steady_clock::now();

    if (emit_frame_) {
      emit_frame_("action_out", std::move(frame));
    }
  }

  // Return immediately — RunLoop re-enters queue_cv_.wait loop.
  // HandleActingResult will be called for each kToolResult observation.
}

// Process tool result received while in kActing state.
// Collects results for parallel tool calls; re-enters thinking only when all are in.
void Controller::HandleActingResult(const Observation& obs) {
  // Try to extract tool_call_id from the result JSON.
  std::string tool_call_id;
  auto id_pos = obs.content.find(R"("tool_call_id":")");
  if (id_pos != std::string::npos) {
    auto val_start = id_pos + 16;
    auto val_end = obs.content.find('"', val_start);
    if (val_end != std::string::npos) {
      tool_call_id = obs.content.substr(val_start, val_end - val_start);
    }
  }

  // If no tool_call_id in result, match by order (fallback for simple dispatchers).
  if (tool_call_id.empty()) {
    for (const auto& tc : pending_tool_calls_) {
      if (pending_results_.find(tc.id) == pending_results_.end()) {
        tool_call_id = tc.id;
        break;
      }
    }
  }

  pending_results_[tool_call_id] = obs.content;

  const bool success = obs.content.find(R"("success":true)") != std::string::npos;

  // Record this tool result in memory.
  MemoryEntry result_entry;
  result_entry.type        = MemoryEntryType::kToolResult;
  result_entry.role        = "tool";
  result_entry.content     = obs.content;
  result_entry.tool_call_id = tool_call_id;
  result_entry.timestamp   = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, result_entry);
  last_activity_ = result_entry.timestamp;
  conversation_active_ = true;

  LOG_INFO("[{}] Tool result received: id=\"{}\" success={} ({}/{})",
           MODULE_NAME, tool_call_id, success,
           pending_results_.size(), pending_tool_calls_.size());

  // Inject <tool_result> marker into the streaming text flow so the UI
  // can render it inline within the same assistant bubble.
  {
    std::string tool_name;
    for (const auto& tc : pending_tool_calls_) {
      if (tc.id == tool_call_id) { tool_name = tc.name; break; }
    }
    std::string marker = "<tool_result>{\"id\":\"" + tool_call_id +
                         "\",\"name\":\"" + tool_name +
                         "\",\"success\":" + (success ? "true" : "false") +
                         ",\"output\":" + obs.content +
                         "}</tool_result>";
    for (const auto& cb : stream_token_callbacks_) {
      cb(marker);
    }

    std::string detail = R"({"id":")" + tool_call_id +
                         R"(","name":")" + tool_name +
                         R"(","success":)" + (success ? "true" : "false") +
                         R"(,"result":)" + obs.content + "}";
    EmitActivity(ActivityKind::kToolResultReceived, std::move(detail));
  }

  // Check if all results are in.
  if (pending_results_.size() < pending_tool_calls_.size()) {
    return;  // Still waiting for more results.
  }

  // All results collected — audit and transition.
  bool all_success = true;
  for (const auto& [id, result_json] : pending_results_) {
    if (result_json.find(R"("success":true)") == std::string::npos) {
      all_success = false;
    }
  }

  PolicyResult audit_result;
  audit_result.outcome = all_success ? PolicyOutcome::kAllow : PolicyOutcome::kDeny;
  audit_result.reason  = all_success ? "all tools succeeded" : "one or more tools failed";
  policy_.AuditAction(session_id_, pending_action_, audit_result);

  TryTransition(all_success ? Event::kActionComplete : Event::kActionFailed);

  // Clear pending state.
  pending_tool_calls_.clear();
  pending_results_.clear();

  Observation continuation;
  continuation.type      = ObservationType::kContinuation;
  continuation.source    = "controller";
  continuation.timestamp = std::chrono::steady_clock::now();
  HandleThinking(continuation);
}

// Deliver response, check stop conditions.
void Controller::HandleResponding(ActionCandidate ac) {
  // Strategy: filter/transform the response text before output.
  ac.response_text = response_filter_->Filter(ac.response_text);

  // If the filter suppressed the response entirely, skip output.
  if (ac.response_text.empty()) {
    LOG_INFO("[{}] Response suppressed by filter", MODULE_NAME);
    TryTransition(Event::kResponseDelivered);
    return;
  }

  LOG_INFO("[{}] Responding: \"{}\"", MODULE_NAME, ac.response_text);
  EmitActivity(ActivityKind::kSpeaking);

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
  last_activity_ = response_entry.timestamp;
  conversation_active_ = true;

  // Check stop conditions.
  if (turn_count_ >= config_.max_turns ||
      total_prompt_tokens_ + total_completion_tokens_ >= config_.token_budget ||
      action_count_ >= config_.action_count_limit ||
      std::chrono::steady_clock::now() - session_start_ >=
          config_.turn_timeout) {
    LOG_INFO("[{}] Stop condition met: turns={}, tokens={}, actions={}",
             MODULE_NAME, turn_count_,
             total_prompt_tokens_ + total_completion_tokens_,
             action_count_);
    TryTransition(Event::kStopConditionMet);  // → Idle
    EmitActivity(ActivityKind::kBudgetExhausted);
  } else {
    TryTransition(Event::kResponseDelivered);  // → Listening
    EmitActivity(ActivityKind::kTurnComplete);
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
      config_.turn_timeout) {
    EmitDiagnostic("Budget exceeded: turn timeout");
    return true;
  }
  return false;
}

void Controller::ResetBudgetWindow() {
  turn_count_ = 0;
  total_prompt_tokens_ = 0;
  total_completion_tokens_ = 0;
  action_count_ = 0;
  first_token_logged_ = false;
  in_thinking_block_ = false;
  thinking_tag_buf_.clear();
  session_start_ = std::chrono::steady_clock::now();
  interrupt_requested_.store(false);
  pending_action_ = ActionCandidate{};
  pending_tool_calls_.clear();
  pending_results_.clear();
  observation_aggregator_->Reset();
  if (tts_segment_) {
    tts_segment_->Reset();
  }
}

// Cancel in-progress work and transition to Listening.
void Controller::HandleInterrupt() {
  interrupt_requested_.store(false);
  LOG_WARN("[{}] Interrupt received in state {}", MODULE_NAME, StateName(state_.load()));
  llm_->Cancel();
  if (cancel_) cancel_();

  // Reset TTS segmentation buffer on interrupt.
  if (tts_segment_) {
    tts_segment_->Reset();
  }

  // Reset observation aggregator buffer on interrupt.
  observation_aggregator_->Reset();

  // Record partial results as MemoryEntry.
  MemoryEntry interrupt_entry;
  interrupt_entry.type = MemoryEntryType::kAssistantMessage;
  interrupt_entry.role = "system";
  interrupt_entry.content = "Turn interrupted";
  interrupt_entry.timestamp = std::chrono::steady_clock::now();
  context_.RecordTurn(session_id_, interrupt_entry);
  last_activity_ = interrupt_entry.timestamp;
  conversation_active_ = true;

  TryTransition(Event::kInterrupt);  // → Listening
  EmitActivity(ActivityKind::kInterrupted);

  EmitDiagnostic("Turn interrupted in state " +
                 std::to_string(static_cast<int>(state_.load())));
}

// Public thread-safe interrupt — requests immediate LLM cancellation and
// enqueues an internal interruption event so RunLoop performs the state
// transition on the loop thread.
void Controller::Interrupt() {
  State current = state_.load();
  if (current != State::kThinking && current != State::kRouting &&
      current != State::kActing) {
    return;  // Not in an interruptible state — no-op.
  }
  interrupt_requested_.store(true);
  llm_->Cancel();
  Observation obs;
  obs.type      = ObservationType::kInterruption;
  obs.content   = "";
  obs.source    = "interrupt";
  obs.timestamp = std::chrono::steady_clock::now();
  EnqueueObservation(std::move(obs));
}

}  // namespace shizuru::core
