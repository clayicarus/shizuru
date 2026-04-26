#include "controller/controller.h"

#include <cassert>
#include <chrono>
#include <exception>
#include <string>
#include <thread>

#include "async_logger.h"
#include "conversation/item.h"
#include "conversation/render.h"
#include "dialogue/default_reducer.h"
#include "io/data_frame.h"

namespace shizuru::core {

namespace {

template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

conversation::ConversationItem EnsureConversationItem(const Observation& obs) {
  if (obs.item.has_value()) { return *obs.item; }

  switch (obs.type) {
    case ObservationType::kUserMessage:
      return conversation::MakeHumanMessageItem(
          obs.source.empty() ? "user" : obs.source, "", obs.content);
    case ObservationType::kSystemEvent:
      return conversation::MakeSystemEventItem(
          obs.source.empty() ? "system:event" : "system:" + obs.source,
          "",
          "event",
          obs.source,
          conversation::ParseJsonOrString(obs.content));
    case ObservationType::kToolResult:
      if (obs.source.rfind("tool:", 0) == 0) {
        return conversation::MakeToolResultItem(
            obs.source.substr(5), "", conversation::ParseJsonOrString(obs.content));
      }
      return conversation::MakeToolResultItem(
          obs.source.empty() ? "tool" : obs.source, "", 
          conversation::ParseJsonOrString(obs.content));
    case ObservationType::kInterruption:
    case ObservationType::kContinuation:
      break;
  }

  return conversation::ConversationItem{};
}

std::string FirstToolCallId(const conversation::ConversationItem& item) {
  if (item.kind != conversation::ItemKind::kToolCall ||
      !item.payload.contains("tool_calls") ||
      !item.payload["tool_calls"].is_array() ||
      item.payload["tool_calls"].empty()) {
    return "";
  }

  const auto& tool_calls = item.payload["tool_calls"];
  if (!tool_calls[0].contains("id")) { return ""; }
  return tool_calls[0]["id"].get<std::string>();
}

MemoryEntry MemoryEntryFromItem(MemoryEntryType type,
                                const conversation::ConversationItem& item,
                                std::chrono::steady_clock::time_point ts) {
  auto rendered = conversation::RenderForLlm(item);

  MemoryEntry entry;
  entry.type = type;
  entry.role = rendered.role;
  entry.content = rendered.content;
  entry.source_tag = rendered.name;
  entry.tool_call_id = rendered.tool_call_id;
  entry.tool_calls_json = rendered.tool_calls_json;
  if (entry.tool_call_id.empty()) {
    entry.tool_call_id = FirstToolCallId(item);
  }
  entry.item_json = conversation::SerializeConversationItem(item);
  entry.timestamp = ts;
  return entry;
}

nlohmann::json ToolCallsToJson(const ActionCandidate& action) {
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& tc : action.tool_calls) {
    nlohmann::json entry;
    entry["id"] = tc.id;
    entry["type"] = "function";
    entry["function"] = {
        {"name", tc.name},
        {"arguments", conversation::ParseJsonOrString(tc.arguments)},
    };
    arr.push_back(std::move(entry));
  }
  return arr;
}

}  // namespace

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
                           : std::make_unique<PassthroughFilter>()) {
  reducer_ = std::make_unique<dialogue::DefaultDialogueReducer>(config_);
}

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
  auto now = std::chrono::steady_clock::now();
  dialogue_state_.session_start = now;
  dialogue_state_.last_activity = now;
  dialogue_state_.conversation_active = false;
  TryTransition(Event::kStart);
  classification_thread_ = std::thread(&Controller::ClassificationWorker, this);
  loop_thread_ = std::thread(&Controller::RunLoop, this);
}

// Request shutdown. Blocks until loop exits.
void Controller::Shutdown() {
  shutdown_requested_.store(true);
  queue_cv_.notify_one();
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
  // Shut down the classification worker thread.
  {
    std::lock_guard<std::mutex> lock(classification_mutex_);
    classification_shutdown_.store(true);
  }
  classification_cv_.notify_one();
  if (classification_thread_.joinable()) {
    classification_thread_.join();
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

// Register callback for structured activity events.
void Controller::OnActivity(ActivityCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnActivity must be called before Start()");
  activity_callbacks_.push_back(std::move(cb));
}

void Controller::OnConversationItem(ConversationItemCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  assert(!loop_thread_.joinable() && "OnConversationItem must be called before Start()");
  conversation_item_callbacks_.push_back(std::move(cb));
}

std::string Controller::NextConversationItemId() {
  return session_id_ + ":item:" +
         std::to_string(++next_conversation_item_seq_);
}

std::string Controller::EnsureAssistantTurnGroupId() {
  if (active_assistant_turn_group_id_.empty()) {
    active_assistant_turn_group_id_ =
        session_id_ + ":assistant_turn:" +
        std::to_string(++next_assistant_turn_seq_);
  }
  return active_assistant_turn_group_id_;
}

void Controller::ResetAssistantTurnUiState() {
  active_assistant_turn_group_id_.clear();
  current_stream_item_id_.clear();
  tool_call_item_ids_.clear();
}

conversation::ConversationItem Controller::StampAssistantTurnItem(
    conversation::ConversationItem item,
    std::string item_id,
    std::string reply_to_item_id) {
  item.conversation_id = session_id_;
  item.turn_group_id = EnsureAssistantTurnGroupId();
  item.item_id = item_id.empty() ? NextConversationItemId() : std::move(item_id);
  if (!reply_to_item_id.empty()) {
    item.reply_to_item_id = std::move(reply_to_item_id);
  }
  return item;
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

void Controller::EmitConversationItem(const conversation::ConversationItem& item,
                                      bool is_delta) {
  for (const auto& cb : conversation_item_callbacks_) {
    cb(item, is_delta);
  }
}

// Main reasoning loop — runs on loop_thread_.
void Controller::RunLoop() {
  while (!shutdown_requested_.load()) {
    Observation obs;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);

      // Compute wait duration from aggregator and TimerBook.
      auto wait_duration = observation_aggregator_->HasPending()
                               ? std::chrono::milliseconds(500)
                               : std::chrono::milliseconds(60000);

      // Use TimerBook::NextDeadline() to compute tighter wait.
      auto next_deadline = timer_book_.NextDeadline();
      if (next_deadline.has_value()) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *next_deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
          remaining = std::chrono::milliseconds(0);
        }
        wait_duration = std::min(wait_duration, remaining);
      }

      queue_cv_.wait_for(lock, wait_duration, [&] {
        return !observation_queue_.empty() ||
               !internal_event_queue_.empty() ||
               shutdown_requested_.load();
      });
      if (shutdown_requested_.load()) break;

      // Harvest expired timers and feed them to the reducer.
      auto now = std::chrono::steady_clock::now();
      auto expired = timer_book_.PopExpired(now);
      for (const auto& entry : expired) {
        // Phase 3: kAggregationTimeout is handled by Controller, not reducer.
        // The reducer can't call the aggregator (purity), so we call
        // CheckTimeout() here and feed the result as AggregationTimeout.
        if (entry.kind == dialogue::TimerKind::kAggregationTimeout) {
          auto timeout_obs = observation_aggregator_->CheckTimeout();
          if (timeout_obs.has_value()) {
            LOG_INFO("[{}] User message received (aggregation timeout): \"{}\"",
                     MODULE_NAME, timeout_obs->content);
            dialogue::AggregationTimeout timeout_event{*timeout_obs, now};
            auto decision = reducer_->Reduce(dialogue_state_, timeout_event);
            lock.unlock();
            ApplyDialogueDecision(decision);
            lock.lock();
          }
          continue;
        }
        dialogue::TimerExpired timer_event{entry.kind, entry.timer_id, now};
        auto decision = reducer_->Reduce(dialogue_state_, timer_event);
        lock.unlock();
        ApplyDialogueDecision(decision);
        lock.lock();
      }

      // Drain internal dialogue events (e.g., async TurnTriggerClassified
      // results) AFTER processing any pending observations.  This ordering
      // is critical for superseding semantics: if a new UserMessageReceived
      // and a stale TurnTriggerClassified are both pending, the observation
      // must be processed first so the reducer's superseding path
      // (HandleUserMessage during kAwaitingTurnTrigger) can cancel the old
      // classification before the stale verdict is applied.
      if (observation_queue_.empty() && !internal_event_queue_.empty()) {
        auto event = std::move(internal_event_queue_.front());
        internal_event_queue_.pop_front();
        auto decision = reducer_->Reduce(dialogue_state_, event);
        lock.unlock();
        ApplyDialogueDecision(decision);
        // Re-enter the loop to check for new observations before processing
        // more internal events.
        continue;
      }

      // Phase 3: Aggregator timeout is now handled by TimerBook
      // (kAggregationTimeout timer fires in the timer harvesting loop above).
      // The old inline CheckTimeout() polling is removed.

      if (observation_queue_.empty()) continue;
      obs = std::move(observation_queue_.front());
      observation_queue_.pop_front();
    }

    // Internal interrupt event (VAD speech_start or explicit Interrupt()):
    // route through the reducer so VAD and text barge-in share the same
    // interrupt semantics (debounce cooldown, CancelLlm effect, etc.).
    State current = state_.load();
    if (obs.type == ObservationType::kInterruption) {
      if (current == State::kThinking || current == State::kRouting ||
          current == State::kActing) {
        auto now = std::chrono::steady_clock::now();
        auto decision = reducer_->Reduce(
            dialogue_state_, dialogue::InterruptRequested{now});
        ApplyDialogueDecision(decision);
      }
      continue;
    }

    // Check for barge-in: if we're in Thinking/Routing/Acting and receive a
    // real user observation, interrupt the current turn and record the new
    // message to context.
    if (obs.type == ObservationType::kUserMessage &&
        (current == State::kThinking || current == State::kRouting ||
         current == State::kActing)) {
      auto now = std::chrono::steady_clock::now();
      // Ask reducer to handle interrupt.
      auto interrupt_decision = reducer_->Reduce(
          dialogue_state_, dialogue::InterruptRequested{now});
      ApplyDialogueDecision(interrupt_decision);
      // Ask reducer to record the interrupting message.
      auto record_decision = reducer_->Reduce(
          dialogue_state_, dialogue::UserMessageReceived{obs, now});
      ApplyDialogueDecision(record_decision);
      // Phase 3: The interrupting message is now in the workspace (buffered
      // during debounce).  Commit it immediately so it's preserved in
      // committed history even if the session ends during debounce.
      if (!dialogue_state_.workspace.user_fragments.empty()) {
        auto& ws = dialogue_state_.workspace;
        for (const auto& frag : ws.user_fragments) {
          Observation frag_obs;
          frag_obs.type = ObservationType::kUserMessage;
          frag_obs.content = frag.content;
          frag_obs.source = frag.source;
          frag_obs.timestamp = frag.timestamp;
          MemoryEntry entry = MemoryEntryFromItem(
              MemoryEntryType::kUserMessage,
              EnsureConversationItem(frag_obs), frag.timestamp);
          context_.RecordTurn(session_id_, entry);
        }
        ws.user_fragments.clear();
        ws.assistant_partial.reset();
      }
      continue;
    }

    // kToolResult branch: resume from kActing when a tool result arrives.
    // Task 10.6: Route through reducer.
    if (state_.load() == State::kActing &&
        obs.type == ObservationType::kToolResult) {
      HandleActingResult(obs);
      continue;
    }

    // Normal flow: if in Listening/Idle and got user observation.
    // Task 10.3: Route through reducer.
    if ((current == State::kListening || current == State::kIdle) &&
        obs.type == ObservationType::kUserMessage) {
      if (current == State::kIdle) {
        // Reset budget when waking from Idle — this was always done in the old code.
        ResetBudgetWindow();
        dialogue_state_.conversation_active = true;
        if (!TryTransition(Event::kStart)) {
          continue;
        }
      }

      // Stage 1: Aggregation (endpointing) — Phase 3: event-driven.
      auto aggregated = observation_aggregator_->Feed(obs);
      if (!aggregated.has_value()) {
        LOG_INFO("[{}] Observation buffered by aggregator: \"{}\"",
                 MODULE_NAME, obs.content);
        EmitDiagnostic("Waiting for more input: \"" + obs.content + "\"");
        EmitActivity(ActivityKind::kBufferingInput, obs.content);
        // Schedule aggregation timeout timer.
        auto agg_now = std::chrono::steady_clock::now();
        timer_book_.Schedule(dialogue::TimerKind::kAggregationTimeout,
                             "aggregation",
                             agg_now + config_.aggregation_timeout);
        continue;  // Stay in kListening, wait for more fragments.
      }

      // Aggregator returned complete observation — cancel timeout timer.
      timer_book_.Cancel("aggregation");

      // Post-interrupt cooldown: after a barge-in, buffer this message
      // via the reducer and wait for more input.
      if (dialogue_state_.cooldown == dialogue::CooldownPhase::kDebouncing) {
        auto now = std::chrono::steady_clock::now();
        dialogue::AggregationComplete agg_event{*aggregated, now};
        auto decision = reducer_->Reduce(dialogue_state_, agg_event);
        ApplyDialogueDecision(decision);
        LOG_INFO("[{}] Post-interrupt cooldown: buffered \"{}\"",
                 MODULE_NAME, aggregated->content);
        continue;  // Stay in kListening, wait for more or timeout.
      }

      // Stage 2: Relevance filter — now handled via StartTurnTriggerClassification effect.
      // Construct AggregationComplete and let the reducer decide.
      auto now = std::chrono::steady_clock::now();
      LOG_INFO("[{}] User message received: \"{}\"",
               MODULE_NAME, aggregated->content);
      dialogue::AggregationComplete agg_event{*aggregated, now};
      auto decision = reducer_->Reduce(dialogue_state_, agg_event);
      ApplyDialogueDecision(decision);
      continue;
    }

    // System events (scheduler reminders, followup check-ins): bypass
    // aggregator and filter, go straight to thinking via reducer.
    // Task 10.8: Route through reducer.
    if ((current == State::kListening || current == State::kIdle) &&
        obs.type == ObservationType::kSystemEvent) {
      if (current == State::kIdle) {
        ResetBudgetWindow();
        dialogue_state_.conversation_active = true;
        if (!TryTransition(Event::kStart)) { continue; }
      }
      LOG_INFO("[{}] System event received: \"{}\"",
               MODULE_NAME, obs.content);
      auto now = std::chrono::steady_clock::now();
      dialogue::SystemEventReceived sys_event{obs, now};
      auto decision = reducer_->Reduce(dialogue_state_, sys_event);
      ApplyDialogueDecision(decision);
      continue;
    }
  }
}

// Build context, submit to LLM with retry, route the result.
void Controller::HandleThinking(const Observation& obs) {
  // Record user messages in memory immediately so every subsequent LLM call
  // in this turn (including after tool denial) sees the original question.
  // Re-enter via kContinuation so BuildContext does not duplicate it.
  if (obs.type == ObservationType::kUserMessage) {
    MemoryEntry user_entry = MemoryEntryFromItem(
        MemoryEntryType::kUserMessage, EnsureConversationItem(obs),
        obs.timestamp);
    context_.RecordTurn(session_id_, user_entry);

    Observation cont;
    cont.type = ObservationType::kContinuation;
    cont.source = obs.source;
    cont.timestamp = obs.timestamp;
    HandleThinking(cont);
    return;
  }

  // System events (reminders, followups) are recorded as user-role messages
  // with source_tag set so the LLM sees name="scheduler" (or other source)
  // in the context window and can distinguish them from real user input.
  if (obs.type == ObservationType::kSystemEvent) {
    MemoryEntry event_entry = MemoryEntryFromItem(
        MemoryEntryType::kUserMessage, EnsureConversationItem(obs),
        obs.timestamp);
    context_.RecordTurn(session_id_, event_entry);

    Observation cont;
    cont.type = ObservationType::kContinuation;
    cont.source = obs.source;
    cont.timestamp = obs.timestamp;
    HandleThinking(cont);
    return;
  }

  // Check budget first.
  if (CheckBudget()) {
    ResetAssistantTurnUiState();
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

  EnsureAssistantTurnGroupId();
  current_stream_item_id_ = NextConversationItemId();

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
          // Emit streaming assistant message as ConversationItem delta.
          // The token is a delta; UI accumulates.
          {
            auto item = StampAssistantTurnItem(
                conversation::MakeAssistantMessageItem(
                    "assistant", "Shizuru", token),
                current_stream_item_id_);
            EmitConversationItem(item, /*is_delta=*/true);
          }
          // TTS segmentation: filter out structured blocks before feeding to TTS.
          // Strips <think>...</think> and any stray legacy blocks from the token stream.
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
                // Check <tool_call> (11 chars) for backward compatibility.
                if (!matched && thinking_tag_buf_.size() >= 11 &&
                    thinking_tag_buf_.substr(thinking_tag_buf_.size() - 11) == "<tool_call>") {
                  in_thinking_block_ = true;
                  if (tts_clean.size() >= 10) tts_clean.erase(tts_clean.size() - 10);
                  else tts_clean.clear();
                  thinking_tag_buf_.clear();
                  matched = true;
                }
                // Check <tool_result> (13 chars) for backward compatibility.
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
        // Task 10.5: Route LLM failure through reducer.
        auto now_ts = std::chrono::steady_clock::now();
        dialogue::LlmFailed fail_event{"LLM submit failed after retries", now_ts};
        auto decision = reducer_->Reduce(dialogue_state_, fail_event);
        ResetAssistantTurnUiState();
        ApplyDialogueDecision(decision);
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

  // Task 10.4: Route LLM completion through reducer.
  auto now_ts = std::chrono::steady_clock::now();
  dialogue::LlmCompleted llm_event;
  llm_event.candidate = std::move(result.candidate);
  llm_event.prompt_tokens = result.prompt_tokens;
  llm_event.completion_tokens = result.completion_tokens;
  llm_event.now = now_ts;
  auto decision = reducer_->Reduce(dialogue_state_, llm_event);
  ApplyDialogueDecision(decision);
}

// NOTE: HandleRouting was removed in Phase 3 — routing is now handled by
// EmitToolCallFrames / DeliverResponse / StartLlm effects from the reducer.

// Emit action/tool_call frames non-blocking; store pending state for HandleActingResult.
// Supports parallel tool calls: emits one frame per tool call, waits for all results.
void Controller::HandleActing(ActionCandidate ac) {
  pending_action_ = ac;
  tool_call_start_ = std::chrono::steady_clock::now();

  // NOTE: Tool call decision is already recorded in committed history by the
  // RecordToolCallDecision effect handler (emitted by the reducer in
  // HandleLlmCompleted).  Do NOT record it again here.

  // Emit one action frame per tool call.
  for (const auto& tc : ac.tool_calls) {
    dialogue_state_.turn_action_count++;
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
    // Emit tool call as ConversationItem for UI.
    {
      auto item = StampAssistantTurnItem(
          conversation::MakeToolCallItem(
              "assistant", "Shizuru",
              nlohmann::json::array({
                  {{"id", tc.id}, {"type", "function"},
                   {"function", {{"name", tc.name},
                                 {"arguments", conversation::ParseJsonOrString(tc.arguments)}}}}
              })),
          {},
          current_stream_item_id_);
      tool_call_item_ids_[tc.id] = item.item_id;
      EmitConversationItem(item, /*is_delta=*/false);
    }

    nlohmann::json request = {
        {"tool_call_id", tc.id},
        {"tool_name", tc.name},
        {"arguments", conversation::ParseJsonOrString(tc.arguments)},
    };
    io::DataFrame frame;
    frame.type = "action/tool_call";
    const auto payload_str = request.dump();
    frame.payload = std::vector<uint8_t>(payload_str.begin(), payload_str.end());
    frame.metadata["tool_call_id"] = tc.id;
    frame.metadata["tool_name"] = tc.name;
    frame.timestamp = std::chrono::steady_clock::now();

    if (emit_frame_) {
      emit_frame_("action_out", std::move(frame));
    }
  }

  // Return immediately — RunLoop re-enters queue_cv_.wait loop.
  // HandleActingResult will be called for each kToolResult observation.
}

// Process tool result received while in kActing state.
// Task 10.6: Route through reducer.
void Controller::HandleActingResult(const Observation& obs) {
  std::string tool_call_id;
  std::string tool_name;
  bool success = false;

  try {
    const auto json = nlohmann::json::parse(obs.content);
    tool_call_id = json.value("tool_call_id", "");
    tool_name = json.value("tool_name", "");
    success = json.value("success", false);
  } catch (...) {
    // Fall through to compatibility parsing below.
  }

  // If no tool_call_id in result, match by order (fallback for simple dispatchers).
  if (tool_call_id.empty()) {
    for (const auto& id : dialogue_state_.pending_tool_call_ids) {
      if (dialogue_state_.pending_tool_results.find(id) ==
          dialogue_state_.pending_tool_results.end()) {
        tool_call_id = id;
        break;
      }
    }
  }

  if (!success) {
    try {
      success = nlohmann::json::parse(obs.content).value("success", false);
    } catch (...) {
      success = false;
    }
  }
  if (tool_name.empty()) {
    for (const auto& tc : pending_action_.tool_calls) {
      if (tc.id == tool_call_id) {
        tool_name = tc.name;
        break;
      }
    }
  }

  LOG_INFO("[{}] Tool result received: id=\"{}\" success={}",
           MODULE_NAME, tool_call_id, success);

  {
    std::string detail = R"({"id":")" + tool_call_id +
                         R"(","name":")" + tool_name +
                         R"(","success":)" + (success ? "true" : "false") +
                         R"(,"result":)" + obs.content + "}";
    EmitActivity(ActivityKind::kToolResultReceived, std::move(detail));
  }

  // Build observation with tool_call_id as source for reducer pairing.
  Observation result_obs = obs;
  result_obs.source = tool_call_id;

  // Route through reducer.
  auto now = std::chrono::steady_clock::now();
  dialogue::ToolResultReceived tool_event{result_obs, now};
  auto decision = reducer_->Reduce(dialogue_state_, tool_event);
  ApplyDialogueDecision(decision);
}

// Deliver response, check stop conditions.
void Controller::HandleResponding(ActionCandidate ac) {
  // Strategy: filter/transform the response text before output.
  ac.response_text = response_filter_->Filter(ac.response_text);

  // If the filter suppressed the response entirely, skip output.
  if (ac.response_text.empty()) {
    LOG_INFO("[{}] Response suppressed by filter", MODULE_NAME);
    TryTransition(Event::kResponseDelivered);
    ResetAssistantTurnUiState();
    return;
  }

  LOG_INFO("[{}] Responding: \"{}\"", MODULE_NAME, ac.response_text);
  EmitActivity(ActivityKind::kSpeaking);

  // Emit final response as complete ConversationItem for UI.
  auto resp_item = StampAssistantTurnItem(
      conversation::MakeAssistantMessageItem(
          "assistant", "Shizuru", ac.response_text),
      current_stream_item_id_);
  EmitConversationItem(resp_item, /*is_delta=*/false);

  // Record response as MemoryEntry.
  MemoryEntry response_entry = MemoryEntryFromItem(
      MemoryEntryType::kAssistantMessage, resp_item,
      std::chrono::steady_clock::now());
  context_.RecordTurn(session_id_, response_entry);
  dialogue_state_.last_activity = response_entry.timestamp;
  dialogue_state_.conversation_active = true;

  // Per-turn budget check — if limits exceeded, enter idle.
  if (dialogue_state_.turn_prompt_tokens + dialogue_state_.turn_completion_tokens >= config_.token_budget ||
      dialogue_state_.turn_action_count >= config_.action_count_limit ||
      dialogue_state_.turn_continuation_count >= config_.max_continuations) {
    LOG_INFO("[{}] Per-turn budget met: llm_calls={}, tokens={}, actions={}, continuations={}",
             MODULE_NAME, dialogue_state_.turn_llm_calls,
             dialogue_state_.turn_prompt_tokens + dialogue_state_.turn_completion_tokens,
             dialogue_state_.turn_action_count,
             dialogue_state_.turn_continuation_count);
    TryTransition(Event::kStopConditionMet);  // → Idle
    EmitActivity(ActivityKind::kBudgetExhausted);
  } else {
    TryTransition(Event::kResponseDelivered);  // → Listening
    EmitActivity(ActivityKind::kTurnComplete);
  }
  ResetAssistantTurnUiState();
}

// Enforce budget guardrails. Returns true if any limit is exceeded.
bool Controller::CheckBudget() {
  if (dialogue_state_.turn_prompt_tokens + dialogue_state_.turn_completion_tokens >=
      config_.token_budget) {
    EmitDiagnostic("Budget exceeded: token budget (" +
                   std::to_string(config_.token_budget) + ")");
    return true;
  }
  if (dialogue_state_.turn_action_count >= config_.action_count_limit) {
    EmitDiagnostic("Budget exceeded: action count limit (" +
                   std::to_string(config_.action_count_limit) + ")");
    return true;
  }
  if (dialogue_state_.turn_continuation_count >= config_.max_continuations) {
    EmitDiagnostic("Budget exceeded: continuation limit (" +
                   std::to_string(config_.max_continuations) + ")");
    return true;
  }
  return false;
}

void Controller::ResetBudgetWindow() {
  dialogue_state_.turn_llm_calls = 0;
  dialogue_state_.turn_prompt_tokens = 0;
  dialogue_state_.turn_completion_tokens = 0;
  dialogue_state_.turn_action_count = 0;
  dialogue_state_.turn_continuation_count = 0;
  first_token_logged_ = false;
  in_thinking_block_ = false;
  thinking_tag_buf_.clear();
  dialogue_state_.session_start = std::chrono::steady_clock::now();
  interrupt_requested_.store(false);
  pending_action_ = ActionCandidate{};
  dialogue_state_.pending_tool_call_ids.clear();
  dialogue_state_.pending_tool_results.clear();
  observation_aggregator_->Reset();
  if (tts_segment_) {
    tts_segment_->Reset();
  }
}

// ---------------------------------------------------------------------------
// Classification worker — runs on classification_thread_.
// Processes turn-trigger classification requests serially, ensuring the
// ObservationFilter is never called concurrently.
// ---------------------------------------------------------------------------

void Controller::ClassificationWorker() {
  while (true) {
    ClassificationRequest req;
    {
      std::unique_lock<std::mutex> lock(classification_mutex_);
      classification_cv_.wait(lock, [&] {
        return !classification_queue_.empty() ||
               classification_shutdown_.load();
      });
      if (classification_shutdown_.load()) return;
      req = std::move(classification_queue_.front());
      classification_queue_.pop_front();
    }

    // Check cancellation before calling the filter — skip work for
    // superseded requests.
    if (req.cancelled->load()) continue;

    bool accepted = observation_filter_->ShouldProcess(req.observation);

    // Check cancellation again after the (potentially slow) filter call.
    // If cancelled while the filter was running, discard the result.
    if (req.cancelled->load()) continue;

    dialogue::TurnTriggerVerdict verdict =
        accepted ? dialogue::TurnTriggerVerdict::kRespondNow
                 : dialogue::TurnTriggerVerdict::kStoreOnly;
    auto now = std::chrono::steady_clock::now();
    dialogue::TurnTriggerClassified classified{req.obs_id, verdict, now};

    if (!shutdown_requested_.load()) {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      internal_event_queue_.push_back(std::move(classified));
    }
    queue_cv_.notify_one();
  }
}

void Controller::ApplyDialogueDecision(
    const dialogue::DialogueDecision& decision) {
  dialogue_state_ = decision.next_state;

  for (const auto& effect : decision.effects) {
    std::visit(overloaded{
      [&](const dialogue::RecordMemory& e) {
        MemoryEntry entry = MemoryEntryFromItem(
            MemoryEntryType::kUserMessage, EnsureConversationItem(e.observation),
            e.observation.timestamp);
        context_.RecordTurn(session_id_, entry);
      },
      [&](const dialogue::CancelLlm&) {
        // Phase 2: CancelLlm no longer records interrupt memory inline.
        // That responsibility moved to RecordInterruptMemory effect.
        interrupt_requested_.store(false);
        llm_->Cancel();
        if (cancel_) cancel_();
        if (tts_segment_) tts_segment_->Reset();
        observation_aggregator_->Reset();
        dialogue_state_.last_activity = std::chrono::steady_clock::now();
        dialogue_state_.conversation_active = true;
        ResetAssistantTurnUiState();
        TryTransition(Event::kInterrupt);
        EmitActivity(ActivityKind::kInterrupted);
        EmitDiagnostic("Turn interrupted in state " +
                       std::to_string(static_cast<int>(state_.load())));
      },
      [&](const dialogue::StartLlmContinuation& e) {
        TryTransition(Event::kUserObservation);
        Observation cont;
        cont.type = ObservationType::kContinuation;
        cont.source = "controller";
        cont.timestamp = e.now;
        try {
          HandleThinking(cont);
        } catch (const std::exception& ex) {
          EmitDiagnostic("Unhandled exception: " + std::string(ex.what()));
          TryTransition(Event::kLlmFailure);
        }
      },
      [&](const dialogue::SignalBudgetExhausted&) {
        ResetAssistantTurnUiState();
        TryTransition(Event::kStopConditionMet);
        EmitActivity(ActivityKind::kBudgetExhausted);
      },
      [&](const dialogue::EmitActivityEffect& e) {
        EmitActivity(e.kind, e.detail);
      },
      [&](const dialogue::NoOp&) {},
      // --- Phase 2 effect handlers (Task 10.1) ---
      [&](const dialogue::StartLlm& e) {
        // Build context + call HandleThinking with trigger observation.
        // Handle FSM transition based on current state.
        State cur = state_.load();
        if (cur == State::kListening || cur == State::kIdle) {
          TryTransition(Event::kUserObservation);
        } else if (cur == State::kActing) {
          // Coming from tool result completion — audit and transition.
          PolicyResult audit_result;
          audit_result.outcome = last_tool_cycle_all_success_
                                     ? PolicyOutcome::kAllow
                                     : PolicyOutcome::kDeny;
          audit_result.reason = last_tool_cycle_all_success_
                                    ? "all tools succeeded"
                                    : "one or more tools failed";
          policy_.AuditAction(session_id_, pending_action_, audit_result);
          TryTransition(last_tool_cycle_all_success_
                            ? Event::kActionComplete
                            : Event::kActionFailed);
        } else if (cur == State::kThinking) {
          // Continuation after LLM returned kContinue — go through Routing.
          TryTransition(Event::kLlmResult);      // kThinking → kRouting
          TryTransition(Event::kRouteToContinue); // kRouting → kThinking
        }
        // If already in kThinking after transitions, proceed.
        try {
          HandleThinking(e.trigger);
        } catch (const std::exception& ex) {
          EmitDiagnostic("Unhandled exception: " + std::string(ex.what()));
          TryTransition(Event::kLlmFailure);
        }
      },
      [&](const dialogue::EmitToolCallFrames& e) {
        // Transition through routing to acting, then emit frames.
        TryTransition(Event::kLlmResult);   // kThinking → kRouting

        // Check policy permission for each tool call (same as old HandleRouting).
        bool all_allowed = true;
        std::string denied_reason;
        for (const auto& tc : e.action.tool_calls) {
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
          TryTransition(Event::kRouteToAction); // kRouting → kActing
          last_tool_cycle_all_success_ = true;
          HandleActing(e.action);
        } else {
          // Denied — record denial as observation and re-enter thinking.
          MemoryEntry denial_entry;
          denial_entry.type = MemoryEntryType::kToolResult;
          denial_entry.role = "system";
          denial_entry.content = "Action denied: " + denied_reason;
          denial_entry.timestamp = std::chrono::steady_clock::now();
          context_.RecordTurn(session_id_, denial_entry);

          TryTransition(Event::kRouteToContinue); // kRouting → kThinking

          // Re-enter thinking via continuation.
          Observation denial_obs;
          denial_obs.type = ObservationType::kContinuation;
          denial_obs.content = "";
          denial_obs.source = "policy";
          denial_obs.timestamp = std::chrono::steady_clock::now();
          try {
            HandleThinking(denial_obs);
          } catch (const std::exception& ex) {
            EmitDiagnostic("Unhandled exception: " + std::string(ex.what()));
            TryTransition(Event::kLlmFailure);
          }
        }
      },
      [&](const dialogue::RecordToolResult& e) {
        // Record tool result memory entry.
        std::string tool_call_id;
        std::string tool_name;
        bool success = false;
        try {
          const auto json = nlohmann::json::parse(e.observation.content);
          tool_call_id = json.value("tool_call_id", "");
          tool_name = json.value("tool_name", "");
          success = json.value("success", false);
        } catch (...) {}

        if (!success) {
          last_tool_cycle_all_success_ = false;
        }

        if (tool_name.empty()) {
          for (const auto& tc : pending_action_.tool_calls) {
            if (tc.id == tool_call_id || tc.id == e.observation.source) {
              tool_name = tc.name;
              break;
            }
          }
        }
        if (tool_call_id.empty()) {
          tool_call_id = e.observation.source;
        }

        const std::string reply_to_item_id =
            tool_call_item_ids_.count(tool_call_id) != 0
                ? tool_call_item_ids_.at(tool_call_id)
                : std::string{};

        auto item = StampAssistantTurnItem(
            conversation::MakeToolResultItem(
                tool_name.empty() ? "tool" : tool_name,
                tool_call_id,
                conversation::ParseJsonOrString(e.observation.content)),
            {},
            reply_to_item_id);
        MemoryEntry result_entry = MemoryEntryFromItem(
            MemoryEntryType::kToolResult, item, std::chrono::steady_clock::now());
        context_.RecordTurn(session_id_, result_entry);
        EmitConversationItem(item, /*is_delta=*/false);
      },
      [&](const dialogue::RecordToolCallDecision& e) {
        // Record tool call decision memory entry.
        auto item = StampAssistantTurnItem(
            conversation::MakeToolCallItem(
                "assistant", "Shizuru", ToolCallsToJson(e.action)),
            {},
            current_stream_item_id_);
        MemoryEntry call_entry = MemoryEntryFromItem(
            MemoryEntryType::kToolCall, item, std::chrono::steady_clock::now());
        context_.RecordTurn(session_id_, call_entry);
      },
      [&](const dialogue::DeliverResponse& e) {
        // Transition through routing to responding, then deliver.
        State cur = state_.load();
        if (cur == State::kThinking) {
          TryTransition(Event::kLlmResult);     // kThinking → kRouting
          TryTransition(Event::kRouteToResponse); // kRouting → kResponding
        }
        HandleResponding(e.action);
      },
      [&](const dialogue::ResetBudgetWindow&) {
        ResetBudgetWindow();
      },
      [&](const dialogue::EmitDiagnosticEffect& e) {
        EmitDiagnostic(e.message);
      },
      [&](const dialogue::ScheduleTimer& e) {
        timer_book_.Schedule(e.kind, e.timer_id, e.deadline);
      },
      [&](const dialogue::CancelTimer& e) {
        timer_book_.Cancel(e.timer_id);
      },
      [&](const dialogue::StartTurnTriggerClassification& e) {
        // Enqueue a classification request for the dedicated worker thread.
        // The worker processes requests serially, so the ObservationFilter
        // is never called concurrently — this is safe for non-thread-safe
        // implementations like LlmObservationFilter.
        //
        // Each request carries a cancellation token.  If a superseding
        // message arrives, CancelTurnTriggerClassification sets the token,
        // and the worker skips the cancelled request (either before calling
        // the filter or after, if the call was already in progress).
        EmitActivity(ActivityKind::kFilteringInput);

        auto cancel_token = std::make_shared<std::atomic<bool>>(false);
        {
          std::lock_guard<std::mutex> lock(classification_mutex_);
          active_cancel_token_ = cancel_token;
          classification_queue_.push_back(ClassificationRequest{
              e.obs_id, e.observation, cancel_token});
        }
        classification_cv_.notify_one();
      },
      [&](const dialogue::CancelTurnTriggerClassification&) {
        // Signal the active classification to skip its result.  The worker
        // checks this flag before and after calling ShouldProcess().
        // This prevents superseded classifications from wasting work on
        // slow filters (e.g., LLM-based) and avoids enqueuing stale results.
        std::lock_guard<std::mutex> lock(classification_mutex_);
        if (active_cancel_token_) {
          active_cancel_token_->store(true);
        }
      },
      [&](const dialogue::TransitionState& e) {
        TryTransition(e.event);
      },
      [&](const dialogue::RecordInterruptMemory&) {
        // Record "Turn interrupted" memory entry — single place for this.
        MemoryEntry interrupt_entry;
        interrupt_entry.type = MemoryEntryType::kAssistantMessage;
        interrupt_entry.role = "system";
        interrupt_entry.content = "Turn interrupted";
        interrupt_entry.timestamp = std::chrono::steady_clock::now();
        context_.RecordTurn(session_id_, interrupt_entry);
      },
      [&](const dialogue::RecordTimeoutResults& e) {
        // Record synthetic timeout result entries for each missing tool call id.
        for (const auto& id : e.missing_tool_call_ids) {
          std::string tool_name;
          for (const auto& tc : pending_action_.tool_calls) {
            if (tc.id == id) {
              tool_name = tc.name;
              break;
            }
          }

          auto item = StampAssistantTurnItem(
              conversation::MakeToolResultItem(
                  tool_name.empty() ? "tool" : tool_name,
                  id,
                  conversation::ParseJsonOrString(
                      R"({"success":false,"error":"tool call timeout"})")),
              {},
              tool_call_item_ids_.count(id) != 0
                  ? tool_call_item_ids_.at(id)
                  : std::string{});
          MemoryEntry timeout_entry = MemoryEntryFromItem(
              MemoryEntryType::kToolResult, item,
              std::chrono::steady_clock::now());
          context_.RecordTurn(session_id_, timeout_entry);
          EmitConversationItem(item, /*is_delta=*/false);
        }
        // Transition out of Acting state so subsequent StartLlm can work.
        if (state_.load() == State::kActing) {
          TryTransition(Event::kActionFailed);
        }
      },
      // --- Phase 3 effect handlers ---
      [&](const dialogue::BufferToWorkspace&) {
        // The reducer already updated next_state.workspace.  This effect
        // exists for explicitness and future auditing — no external side
        // effects needed here.
      },
      [&](const dialogue::CommitWorkspace& e) {
        auto& ws = dialogue_state_.workspace;
        if (e.merge_fragments && !ws.user_fragments.empty()) {
          // Merge all user fragments into a single MemoryEntry.
          std::string merged_content;
          std::string merged_source;
          auto latest_ts = ws.user_fragments.front().timestamp;
          for (const auto& frag : ws.user_fragments) {
            if (!merged_content.empty()) merged_content += " ";
            merged_content += frag.content;
            merged_source = frag.source;
            if (frag.timestamp > latest_ts) latest_ts = frag.timestamp;
          }
          Observation merged_obs;
          merged_obs.type = ObservationType::kUserMessage;
          merged_obs.content = merged_content;
          merged_obs.source = merged_source;
          merged_obs.timestamp = latest_ts;
          MemoryEntry entry = MemoryEntryFromItem(
              MemoryEntryType::kUserMessage,
              EnsureConversationItem(merged_obs), latest_ts);
          context_.RecordTurn(session_id_, entry);
        } else if (!e.merge_fragments) {
          for (const auto& frag : ws.user_fragments) {
            Observation frag_obs;
            frag_obs.type = ObservationType::kUserMessage;
            frag_obs.content = frag.content;
            frag_obs.source = frag.source;
            frag_obs.timestamp = frag.timestamp;
            MemoryEntry entry = MemoryEntryFromItem(
                MemoryEntryType::kUserMessage,
                EnsureConversationItem(frag_obs), frag.timestamp);
            context_.RecordTurn(session_id_, entry);
          }
        }
        if (ws.assistant_partial.has_value()) {
          Observation asst_obs;
          asst_obs.type = ObservationType::kContinuation;
          asst_obs.content = ws.assistant_partial->content;
          asst_obs.source = ws.assistant_partial->source;
          asst_obs.timestamp = ws.assistant_partial->timestamp;
          MemoryEntry entry = MemoryEntryFromItem(
              MemoryEntryType::kAssistantMessage,
              EnsureConversationItem(asst_obs),
              ws.assistant_partial->timestamp);
          context_.RecordTurn(session_id_, entry);
        }
        // Clear workspace.
        ws.user_fragments.clear();
        ws.assistant_partial.reset();
      },
      [&](const dialogue::DiscardWorkspace&) {
        dialogue_state_.workspace.user_fragments.clear();
        dialogue_state_.workspace.assistant_partial.reset();
      },
    }, effect);
  }
}

// NOTE: HandleInterrupt() was removed in Phase 3 — interrupt handling is now
// done by the reducer (InterruptRequested event → CancelLlm + RecordInterruptMemory
// + ScheduleTimer effects).

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
