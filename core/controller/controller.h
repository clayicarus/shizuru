#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "context/context_strategy.h"
#include "controller/config.h"
#include "controller/types.h"
#include "core/conversation_item.h"
#include "dialogue/reducer.h"
#include "dialogue/timer_book.h"
#include "dialogue/types.h"
#include "interfaces/llm_client.h"
#include "io/io_device.h"
#include "policy/policy_layer.h"
#include "strategies/response_filter.h"
#include "strategies/tts_segment_strategy.h"

namespace shizuru::core {

// Hash helper for std::pair<State, Event> used as key in the transition table.
struct PairHash {
  std::size_t operator()(const std::pair<State, Event>& p) const {
    auto h1 = std::hash<int>{}(static_cast<int>(p.first));
    auto h2 = std::hash<int>{}(static_cast<int>(p.second));
    return h1 ^ (h2 << 16);
  }
};

class Controller {
 public:
  // Injected by CoreDevice; called when Controller needs to cancel in-progress IO.
  using CancelCallback = std::function<void()>;

  // Injected by CoreDevice; called when Controller wants to emit a DataFrame.
  using EmitFrameCallback = std::function<void(const std::string& port, io::DataFrame)>;

  // All dependencies injected via constructor.
  Controller(std::string session_id,
             ControllerConfig config,
             std::unique_ptr<LlmClient> llm,
             EmitFrameCallback emit_frame,
             CancelCallback cancel,
             ContextStrategy& context,
             PolicyLayer& policy,
             std::unique_ptr<TtsSegmentStrategy> tts_segment = nullptr,
             std::unique_ptr<ResponseFilter> response_filter = nullptr);

  ~Controller();

  // Thread-safe: enqueue a conversation item from any thread.
  void EnqueueItem(ConversationItem item);

  // Thread-safe: enqueue a tool result from any thread.
  void EnqueueToolResult(ConversationItem item);

  // Thread-safe: enqueue a system event from any thread.
  void EnqueueSystemEvent(ConversationItem item);

  // Start the reasoning loop on its own thread.
  void Start();

  // Request shutdown (thread-safe). Blocks until loop exits.
  void Shutdown();

  // Thread-safe state accessor.
  State GetState() const;

  // Register callbacks for state transitions.
  using TransitionCallback =
      std::function<void(State from, State to, Event event)>;
  void OnTransition(TransitionCallback cb);

  // Request an interrupt from outside the loop thread (e.g. VAD speech_start).
  void Interrupt();

  // Recover from kError state to kIdle. Thread-safe.
  void Recover();

  // Register callback for diagnostic events.
  using DiagnosticCallback = std::function<void(const std::string& message)>;
  void OnDiagnostic(DiagnosticCallback cb);

  // Register callback for structured activity events (UI consumption).
  using ActivityCallback = std::function<void(const ActivityEvent& event)>;
  void OnActivity(ActivityCallback cb);

  // Register callback for conversation items (unified UI data source).
  using ConversationItemCallback =
      std::function<void(const ConversationItem& item, bool is_delta)>;
  void OnConversationItem(ConversationItemCallback cb);

 private:
  static constexpr char MODULE_NAME[] = "Controller";

  std::string session_id_;

  // Internal event wrapper for the queue.
  enum class InternalEventKind {
    kConversationItem,
    kToolResult,
    kSystemEvent,
    kInterrupt,
  };
  struct InternalEvent {
    InternalEventKind kind;
    ConversationItem item;
  };

  void RunLoop();
  bool TryTransition(Event event);
  void HandleThinking();
  void HandleActing(ActionCandidate ac);
  void HandleActingResult(const ConversationItem& item);
  void HandleResponding(ActionCandidate ac);
  bool CheckBudget();
  void ResetBudgetWindow();
  void ApplyDialogueDecision(const dialogue::DialogueDecision& decision);
  void EmitDiagnostic(const std::string& message);
  void EmitActivity(ActivityKind kind, std::string detail = {});
  void EmitConversationItemCb(const ConversationItem& item, bool is_delta);
  std::string NextConversationItemId();
  std::string EnsureAssistantTurnGroupId();
  void ResetAssistantTurnUiState();

  // Static transition table
  static const std::unordered_map<std::pair<State, Event>, State, PairHash>
      kTransitionTable;

  ControllerConfig config_;
  std::unique_ptr<LlmClient> llm_;
  EmitFrameCallback emit_frame_;
  CancelCallback cancel_;
  ContextStrategy& context_;
  PolicyLayer& policy_;

  // Pluggable strategies (owned by Controller).
  std::unique_ptr<TtsSegmentStrategy> tts_segment_;
  std::unique_ptr<ResponseFilter> response_filter_;

  // Pending tool call state.
  ActionCandidate pending_action_;
  std::chrono::steady_clock::time_point tool_call_start_;
  bool last_tool_cycle_all_success_ = true;

  // State (accessed from loop thread; read via atomic for external queries)
  std::atomic<State> state_{State::kIdle};

  // Event queue (cross-thread)
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<InternalEvent> event_queue_;

  // Session counters
  bool first_token_logged_ = false;
  bool in_thinking_block_ = false;
  std::string thinking_tag_buf_;
  uint64_t next_conversation_item_seq_ = 0;
  uint64_t next_assistant_turn_seq_ = 0;
  std::string active_assistant_turn_group_id_;
  std::string current_stream_item_id_;
  std::unordered_map<std::string, std::string> tool_call_item_ids_;

  // Dialogue reducer
  std::unique_ptr<dialogue::DialogueReducer> reducer_;
  dialogue::DialogueState dialogue_state_;
  dialogue::TimerBook timer_book_;

  // Loop thread
  std::thread loop_thread_;
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> interrupt_requested_{false};

  // Callbacks (must be registered before Start())
  std::mutex callbacks_mutex_;
  std::vector<TransitionCallback> transition_callbacks_;
  std::vector<DiagnosticCallback> diagnostic_callbacks_;
  std::vector<ActivityCallback> activity_callbacks_;
  std::vector<ConversationItemCallback> conversation_item_callbacks_;
};

}  // namespace shizuru::core
