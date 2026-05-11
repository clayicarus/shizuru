#include "controller/controller.h"

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "async_logger.h"
#include "core/invoke_batch.h"
#include "core/provider_render.h"
#include "dialogue/default_reducer.h"

namespace shizuru::core {

namespace {

template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

// Static transition table
const std::unordered_map<std::pair<State, Event>, State, PairHash>
    Controller::kTransitionTable = {
        {{State::kIdle, Event::kStart}, State::kListening},
        {{State::kIdle, Event::kShutdown}, State::kTerminated},
        {{State::kListening, Event::kUserObservation}, State::kThinking},
        {{State::kListening, Event::kShutdown}, State::kTerminated},
        {{State::kListening, Event::kStop}, State::kIdle},
        {{State::kThinking, Event::kLlmResult}, State::kRouting},
        {{State::kThinking, Event::kLlmFailure}, State::kError},
        {{State::kThinking, Event::kInterrupt}, State::kListening},
        {{State::kThinking, Event::kStopConditionMet}, State::kIdle},
        {{State::kThinking, Event::kShutdown}, State::kTerminated},
        {{State::kRouting, Event::kRouteToAction}, State::kActing},
        {{State::kRouting, Event::kRouteToResponse}, State::kResponding},
        {{State::kRouting, Event::kRouteToContinue}, State::kThinking},
        {{State::kRouting, Event::kInterrupt}, State::kListening},
        {{State::kRouting, Event::kShutdown}, State::kTerminated},
        {{State::kActing, Event::kActionComplete}, State::kThinking},
        {{State::kActing, Event::kActionFailed}, State::kThinking},
        {{State::kActing, Event::kInterrupt}, State::kListening},
        {{State::kActing, Event::kShutdown}, State::kTerminated},
        {{State::kResponding, Event::kResponseDelivered}, State::kListening},
        {{State::kResponding, Event::kStopConditionMet}, State::kIdle},
        {{State::kResponding, Event::kShutdown}, State::kTerminated},
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
                       std::unique_ptr<TtsSegmentStrategy> tts_segment,
                       std::unique_ptr<ResponseFilter> response_filter)
    : session_id_(std::move(session_id)),
      config_(std::move(config)),
      llm_(std::move(llm)),
      emit_frame_(std::move(emit_frame)),
      cancel_(std::move(cancel)),
      context_(context),
      policy_(policy),
      tts_segment_(std::move(tts_segment)),
      response_filter_(response_filter
                           ? std::move(response_filter)
                           : std::make_unique<PassthroughFilter>()) {
  reducer_ = std::make_unique<dialogue::DefaultDialogueReducer>(config_);

  auto epoch_ns = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  next_conversation_item_seq_ = epoch_ns % 1000000000ULL;
  next_assistant_turn_seq_ = epoch_ns % 1000000000ULL;
}

Controller::~Controller() {
  if (loop_thread_.joinable()) {
    Shutdown();
  }
}

void Controller::EnqueueItem(ConversationItem item) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    event_queue_.push_back(InternalEvent{InternalEventKind::kConversationItem, std::move(item)});
  }
  queue_cv_.notify_one();
}

void Controller::EnqueueToolResult(ConversationItem item) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    event_queue_.push_back(InternalEvent{InternalEventKind::kToolResult, std::move(item)});
  }
  queue_cv_.notify_one();
}

void Controller::EnqueueSystemEvent(ConversationItem item) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    event_queue_.push_back(InternalEvent{InternalEventKind::kSystemEvent, std::move(item)});
  }
  queue_cv_.notify_one();
}

void Controller::Start() {
  auto now = std::chrono::steady_clock::now();
  dialogue_state_.session_start = now;
  dialogue_state_.last_activity = now;
  dialogue_state_.conversation_active = false;
  TryTransition(Event::kStart);
  loop_thread_ = std::thread(&Controller::RunLoop, this);
}

void Controller::Shutdown() {
  shutdown_requested_.store(true);
  queue_cv_.notify_one();
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
  TryTransition(Event::kShutdown);
}

State Controller::GetState() const {
  return state_.load();
}

void Controller::OnTransition(TransitionCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  transition_callbacks_.push_back(std::move(cb));
}

void Controller::OnDiagnostic(DiagnosticCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  diagnostic_callbacks_.push_back(std::move(cb));
}

void Controller::OnActivity(ActivityCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
  activity_callbacks_.push_back(std::move(cb));
}

void Controller::OnConversationItem(ConversationItemCallback cb) {
  std::lock_guard<std::mutex> lock(callbacks_mutex_);
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
    event_queue_.clear();
  }

  for (const auto& cb : transition_callbacks_) {
    cb(old_state, new_state, event);
  }

  policy_.AuditTransition(session_id_, old_state, new_state, event);
  return true;
}

void Controller::EmitDiagnostic(const std::string& message) {
  for (const auto& cb : diagnostic_callbacks_) {
    cb(message);
  }
}

void Controller::EmitActivity(ActivityKind kind, std::string detail) {
  ActivityEvent event{kind, std::move(detail)};
  for (const auto& cb : activity_callbacks_) {
    cb(event);
  }
}

void Controller::EmitConversationItemCb(const ConversationItem& item,
                                        bool is_delta) {
  for (const auto& cb : conversation_item_callbacks_) {
    cb(item, is_delta);
  }
}

// Main reasoning loop
void Controller::RunLoop() {
  while (!shutdown_requested_.load()) {
    InternalEvent evt;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);

      auto wait_duration = std::chrono::milliseconds(60000);
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
        return !event_queue_.empty() || shutdown_requested_.load();
      });
      if (shutdown_requested_.load()) break;

      // Harvest expired timers.
      auto now = std::chrono::steady_clock::now();
      auto expired = timer_book_.PopExpired(now);
      for (const auto& entry : expired) {
        dialogue::TimerExpired timer_event{entry.kind, entry.timer_id, now};
        auto decision = reducer_->Reduce(dialogue_state_, timer_event);
        lock.unlock();
        ApplyDialogueDecision(decision);
        lock.lock();
      }

      if (event_queue_.empty()) continue;
      evt = std::move(event_queue_.front());
      event_queue_.pop_front();
    }

    State current = state_.load();

    // Handle interrupt events.
    if (evt.kind == InternalEventKind::kInterrupt) {
      if (current == State::kThinking || current == State::kRouting ||
          current == State::kActing) {
        auto now = std::chrono::steady_clock::now();
        auto decision = reducer_->Reduce(
            dialogue_state_, dialogue::InterruptRequested{now});
        ApplyDialogueDecision(decision);
      }
      continue;
    }

    // Handle tool results.
    if (evt.kind == InternalEventKind::kToolResult &&
        state_.load() == State::kActing) {
      HandleActingResult(evt.item);
      continue;
    }

    // Handle conversation items (user messages).
    if (evt.kind == InternalEventKind::kConversationItem) {
      // Barge-in: interrupt if in thinking/routing/acting.
      if (current == State::kThinking || current == State::kRouting ||
          current == State::kActing) {
        auto now = std::chrono::steady_clock::now();
        auto interrupt_decision = reducer_->Reduce(
            dialogue_state_, dialogue::InterruptRequested{now});
        ApplyDialogueDecision(interrupt_decision);
        // Record the interrupting item.
        auto record_decision = reducer_->Reduce(
            dialogue_state_,
            dialogue::ConversationItemReceived{evt.item, now});
        ApplyDialogueDecision(record_decision);
        continue;
      }

      if (current == State::kListening || current == State::kIdle) {
        if (current == State::kIdle) {
          ResetBudgetWindow();
          dialogue_state_.conversation_active = true;
          if (!TryTransition(Event::kStart)) { continue; }
        }

        // Post-interrupt cooldown: buffer via reducer.
        if (dialogue_state_.cooldown == dialogue::CooldownPhase::kDebouncing) {
          auto now = std::chrono::steady_clock::now();
          auto decision = reducer_->Reduce(
              dialogue_state_,
              dialogue::ConversationItemReceived{evt.item, now});
          ApplyDialogueDecision(decision);
          continue;
        }

        // Normal path: route through reducer.
        auto now = std::chrono::steady_clock::now();
        auto decision = reducer_->Reduce(
            dialogue_state_,
            dialogue::ConversationItemReceived{evt.item, now});
        ApplyDialogueDecision(decision);
        continue;
      }
    }

    // Handle system events.
    if (evt.kind == InternalEventKind::kSystemEvent) {
      if (current == State::kListening || current == State::kIdle) {
        if (current == State::kIdle) {
          ResetBudgetWindow();
          dialogue_state_.conversation_active = true;
          if (!TryTransition(Event::kStart)) { continue; }
        }
        auto now = std::chrono::steady_clock::now();
        auto decision = reducer_->Reduce(
            dialogue_state_,
            dialogue::SystemEventReceived{evt.item, now});
        ApplyDialogueDecision(decision);
        continue;
      }
    }
  }
}

// Build context and submit to LLM.
void Controller::HandleThinking() {
  if (CheckBudget()) {
    ResetAssistantTurnUiState();
    TryTransition(Event::kStopConditionMet);
    EmitActivity(ActivityKind::kBudgetExhausted);
    return;
  }

  if (interrupt_requested_.load()) { return; }

  EnsureAssistantTurnGroupId();
  current_stream_item_id_ = NextConversationItemId();

  // Build context: get history from store and render via provider_render.
  auto& store = context_.GetStore();
  auto history = store.GetWindow(session_id_, context_.GetConfig().max_context_tokens);
  std::string system_instruction = context_.GetSystemInstruction(session_id_);

  // Use provider_render to project history into OpenAI messages JSON.
  core::InvokeBatch empty_batch;
  empty_batch.conversation_id = session_id_;
  nlohmann::json messages = services::RenderMessages(
      history, empty_batch, system_instruction);

  first_token_logged_ = false;
  in_thinking_block_ = false;
  thinking_tag_buf_.clear();

  LOG_DEBUG("[{}] Context built: {} messages", MODULE_NAME, messages.size());

  // Submit to LLM with retry.
  LlmResult result;
  bool success = false;
  for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
    try {
      if (attempt == 0) {
        EmitActivity(ActivityKind::kThinkingStarted);
      } else {
        EmitActivity(ActivityKind::kThinkingRetry, std::to_string(attempt + 1));
      }

      if (config_.use_streaming) {
        result = llm_->SubmitStreaming(messages, [this](const std::string& token) {
          if (interrupt_requested_.load()) { return; }
          if (!first_token_logged_) {
            LOG_INFO("[{}] LLM first token received", MODULE_NAME);
            first_token_logged_ = true;
          }
          // TODO: TTS segmentation and streaming ConversationItem deltas.
        });
        if (interrupt_requested_.load()) { return; }
      } else {
        result = llm_->Submit(messages);
        if (interrupt_requested_.load()) { return; }
      }
      success = true;
      break;
    } catch (...) {
      if (interrupt_requested_.load()) { return; }
      if (attempt == config_.max_retries) {
        auto now_ts = std::chrono::steady_clock::now();
        dialogue::LlmFailed fail_event{"LLM submit failed after retries", now_ts};
        auto decision = reducer_->Reduce(dialogue_state_, fail_event);
        ResetAssistantTurnUiState();
        ApplyDialogueDecision(decision);
        return;
      }
      auto delay = config_.retry_base_delay * (1 << attempt);
      std::this_thread::sleep_for(delay);
    }
  }

  if (!success) return;

  // Route LLM completion through reducer.
  auto now_ts = std::chrono::steady_clock::now();
  dialogue::LlmCompleted llm_event;
  llm_event.candidate = std::move(result.candidate);
  llm_event.prompt_tokens = result.prompt_tokens;
  llm_event.completion_tokens = result.completion_tokens;
  llm_event.now = now_ts;
  auto decision = reducer_->Reduce(dialogue_state_, llm_event);
  ApplyDialogueDecision(decision);
}

// Emit tool call frames.
void Controller::HandleActing(ActionCandidate ac) {
  pending_action_ = ac;
  tool_call_start_ = std::chrono::steady_clock::now();

  for (const auto& tc : ac.tool_calls) {
    dialogue_state_.turn_action_count++;
    LOG_INFO("[{}] Acting: tool=\"{}\" id=\"{}\"", MODULE_NAME, tc.name, tc.id);
    EmitActivity(ActivityKind::kToolDispatched, tc.name);

    nlohmann::json request = {
        {"tool_call_id", tc.id},
        {"tool_name", tc.name},
        {"arguments", nlohmann::json::parse(tc.arguments, nullptr, false)},
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
}

// Process tool result.
void Controller::HandleActingResult(const ConversationItem& item) {
  auto now = std::chrono::steady_clock::now();

  // Extract tool_call_id from item.
  std::string tool_call_id = item.item_id;

  LOG_INFO("[{}] Tool result received: id=\"{}\"", MODULE_NAME, tool_call_id);
  EmitActivity(ActivityKind::kToolResultReceived, tool_call_id);

  // Route through reducer.
  dialogue::ToolResultReceived tool_event{item, now};
  auto decision = reducer_->Reduce(dialogue_state_, tool_event);
  ApplyDialogueDecision(decision);
}

// Deliver response.
void Controller::HandleResponding(ActionCandidate ac) {
  ac.response_text = response_filter_->Filter(ac.response_text);

  if (ac.response_text.empty()) {
    TryTransition(Event::kResponseDelivered);
    ResetAssistantTurnUiState();
    return;
  }

  LOG_INFO("[{}] Responding: \"{}\"", MODULE_NAME, ac.response_text);
  EmitActivity(ActivityKind::kSpeaking);

  // Emit response frame.
  if (emit_frame_) {
    io::DataFrame frame;
    frame.type = "text/plain";
    frame.payload = std::vector<uint8_t>(
        ac.response_text.begin(), ac.response_text.end());
    frame.metadata["tts_final"] = "1";
    frame.timestamp = std::chrono::steady_clock::now();
    emit_frame_("tts_out", std::move(frame));
  }

  // Record response in history.
  ConversationItem resp_item;
  resp_item.kind = ConversationItemKind::kAssistantMessage;
  resp_item.item_id = NextConversationItemId();
  resp_item.conversation_id = session_id_;
  resp_item.actor = ActorRef{"assistant", "Shizuru", ActorKind::kAssistant};
  resp_item.parts = {TextPart{ac.response_text}};
  resp_item.wall_time = std::chrono::system_clock::now();

  context_.GetStore().Append(session_id_, resp_item);
  EmitConversationItemCb(resp_item, false);

  dialogue_state_.last_activity = std::chrono::steady_clock::now();
  dialogue_state_.conversation_active = true;

  // Budget check.
  if (dialogue_state_.turn_prompt_tokens + dialogue_state_.turn_completion_tokens >= config_.token_budget ||
      dialogue_state_.turn_action_count >= config_.action_count_limit ||
      dialogue_state_.turn_continuation_count >= config_.max_continuations) {
    TryTransition(Event::kStopConditionMet);
    EmitActivity(ActivityKind::kBudgetExhausted);
  } else {
    TryTransition(Event::kResponseDelivered);
    EmitActivity(ActivityKind::kTurnComplete);
  }
  ResetAssistantTurnUiState();
}

bool Controller::CheckBudget() {
  if (dialogue_state_.turn_prompt_tokens + dialogue_state_.turn_completion_tokens >=
      config_.token_budget) {
    return true;
  }
  if (dialogue_state_.turn_action_count >= config_.action_count_limit) {
    return true;
  }
  if (dialogue_state_.turn_continuation_count >= config_.max_continuations) {
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
  if (tts_segment_) { tts_segment_->Reset(); }
}

void Controller::ApplyDialogueDecision(
    const dialogue::DialogueDecision& decision) {
  dialogue_state_ = decision.next_state;

  for (const auto& effect : decision.effects) {
    std::visit(overloaded{
      [&](const dialogue::CancelLlm&) {
        interrupt_requested_.store(false);
        llm_->Cancel();
        if (cancel_) cancel_();
        if (tts_segment_) tts_segment_->Reset();
        dialogue_state_.last_activity = std::chrono::steady_clock::now();
        dialogue_state_.conversation_active = true;
        ResetAssistantTurnUiState();
        TryTransition(Event::kInterrupt);
        EmitActivity(ActivityKind::kInterrupted);
      },
      [&](const dialogue::StartLlmContinuation& e) {
        (void)e;
        State cur = state_.load();
        if (cur == State::kListening || cur == State::kIdle) {
          TryTransition(Event::kUserObservation);
        } else if (cur == State::kActing) {
          TryTransition(last_tool_cycle_all_success_
                            ? Event::kActionComplete
                            : Event::kActionFailed);
        } else if (cur == State::kThinking) {
          TryTransition(Event::kLlmResult);
          TryTransition(Event::kRouteToContinue);
        }
        try {
          HandleThinking();
        } catch (...) {
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
      [&](const dialogue::EmitToolCallFrames& e) {
        TryTransition(Event::kLlmResult);

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
            all_allowed = false;
            denied_reason = permission.reason;
            break;
          }
        }

        if (all_allowed) {
          TryTransition(Event::kRouteToAction);
          last_tool_cycle_all_success_ = true;
          HandleActing(e.action);
        } else {
          TryTransition(Event::kRouteToContinue);
          // Record denial in history.
          ConversationItem denial_item;
          denial_item.kind = ConversationItemKind::kSystemEvent;
          denial_item.item_id = NextConversationItemId();
          denial_item.conversation_id = session_id_;
          denial_item.actor = ActorRef{"system", "System", ActorKind::kSystem};
          denial_item.parts = {TextPart{"Action denied: " + denied_reason}};
          denial_item.wall_time = std::chrono::system_clock::now();
          context_.GetStore().Append(session_id_, denial_item);
          try {
            HandleThinking();
          } catch (...) {
            TryTransition(Event::kLlmFailure);
          }
        }
      },
      [&](const dialogue::RecordToolCallDecision& e) {
        // Record tool call in history with proper ToolCallParts.
        ConversationItem tc_item;
        tc_item.kind = ConversationItemKind::kToolCall;
        tc_item.item_id = NextConversationItemId();
        tc_item.conversation_id = session_id_;
        tc_item.actor = ActorRef{"assistant", "Shizuru", ActorKind::kAssistant};
        for (const auto& tc : e.action.tool_calls) {
          tc_item.parts.emplace_back(ToolCallPart{tc.id, tc.name, tc.arguments});
        }
        tc_item.wall_time = std::chrono::system_clock::now();
        context_.GetStore().Append(session_id_, tc_item);
        EmitConversationItemCb(tc_item, false);
      },
      [&](const dialogue::DeliverResponse& e) {
        State cur = state_.load();
        if (cur == State::kThinking) {
          TryTransition(Event::kLlmResult);
          TryTransition(Event::kRouteToResponse);
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
      [&](const dialogue::CancelTurnTriggerClassification&) {
        // Turn-trigger filtering currently disabled — no-op.
      },
      [&](const dialogue::TransitionState& e) {
        TryTransition(e.event);
      },
      [&](const dialogue::RecordInterruptMemory&) {
        ConversationItem interrupt_item;
        interrupt_item.kind = ConversationItemKind::kSystemEvent;
        interrupt_item.item_id = NextConversationItemId();
        interrupt_item.conversation_id = session_id_;
        interrupt_item.actor = ActorRef{"system", "System", ActorKind::kSystem};
        interrupt_item.parts = {TextPart{"Turn interrupted"}};
        interrupt_item.wall_time = std::chrono::system_clock::now();
        context_.GetStore().Append(session_id_, interrupt_item);
      },
      [&](const dialogue::RecordTimeoutResults& e) {
        for (const auto& id : e.missing_tool_call_ids) {
          ConversationItem timeout_item;
          timeout_item.kind = ConversationItemKind::kToolResult;
          timeout_item.item_id = id;
          timeout_item.conversation_id = session_id_;
          timeout_item.actor = ActorRef{"tool", "tool", ActorKind::kTool};
          timeout_item.parts = {TextPart{R"({"success":false,"error":"tool call timeout"})"}};
          timeout_item.wall_time = std::chrono::system_clock::now();
          context_.GetStore().Append(session_id_, timeout_item);
        }
        if (state_.load() == State::kActing) {
          TryTransition(Event::kActionFailed);
        }
      },
      [&](const dialogue::RecordConversationItem& e) {
        context_.GetStore().Append(session_id_, e.item);
      },
      [&](const dialogue::StartLlmWithBatch& e) {
        // Record items then think.
        for (const auto& item : e.items) {
          context_.GetStore().Append(session_id_, item);
        }
        State cur = state_.load();
        if (cur == State::kListening || cur == State::kIdle) {
          TryTransition(Event::kUserObservation);
        }
        try {
          HandleThinking();
        } catch (...) {
          TryTransition(Event::kLlmFailure);
        }
      },
      [&](const dialogue::RecordToolResultItem& e) {
        context_.GetStore().Append(session_id_, e.item);
        EmitConversationItemCb(e.item, false);
      },
    }, effect);
  }
}

void Controller::Interrupt() {
  State current = state_.load();
  if (current != State::kThinking && current != State::kRouting &&
      current != State::kActing) {
    return;
  }
  interrupt_requested_.store(true);
  llm_->Cancel();
  // Enqueue interrupt event.
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    event_queue_.push_back(InternalEvent{InternalEventKind::kInterrupt, {}});
  }
  queue_cv_.notify_one();
}

void Controller::Recover() {
  if (state_.load() != State::kError) { return; }
  TryTransition(Event::kRecover);
}

}  // namespace shizuru::core
