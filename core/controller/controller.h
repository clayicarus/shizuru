#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include "interfaces/llm_client.h"
#include "io/data_frame.h"
#include "policy/policy_layer.h"
#include "strategies/observation_aggregator.h"
#include "strategies/observation_filter.h"
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
  // session_id must match the key used to initialize ContextStrategy and
  // PolicyLayer (via InitSession), so all lookups resolve to the same slot.
  //
  // Strategy pointers are optional — if null, defaults are used:
  //   observation_aggregator → PassthroughAggregator (no buffering)
  //   observation_filter     → AcceptAllFilter (process everything)
  //   tts_segment            → nullptr (no TTS segmentation)
  //   response_filter        → PassthroughFilter (no transformation)
  Controller(std::string session_id,
             ControllerConfig config,
             std::unique_ptr<LlmClient> llm,
             EmitFrameCallback emit_frame,
             CancelCallback cancel,
             ContextStrategy& context,
             PolicyLayer& policy,
             std::unique_ptr<ObservationAggregator> observation_aggregator = nullptr,
             std::unique_ptr<ObservationFilter> observation_filter = nullptr,
             std::unique_ptr<TtsSegmentStrategy> tts_segment = nullptr,
             std::unique_ptr<ResponseFilter> response_filter = nullptr);

  ~Controller();

  // Thread-safe: enqueue an observation from any thread.
  void EnqueueObservation(Observation obs);

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
  // Thread-safe. No-op if not in an interruptible state.
  void Interrupt();

  // Register callback for diagnostic events.
  using DiagnosticCallback = std::function<void(const std::string& message)>;
  void OnDiagnostic(DiagnosticCallback cb);

  // Register callback for assistant text responses.
  using ResponseCallback = std::function<void(const ActionCandidate& response)>;
  void OnResponse(ResponseCallback cb);

  // Register callback for streaming token deltas (only fires when use_streaming=true).
  using StreamTokenCallback = std::function<void(const std::string& token)>;
  void OnStreamToken(StreamTokenCallback cb);

  // Register callback for structured activity events (UI consumption).
  using ActivityCallback = std::function<void(const ActivityEvent& event)>;
  void OnActivity(ActivityCallback cb);

 private:
  static constexpr char MODULE_NAME[] = "Controller";

  std::string session_id_;

  void RunLoop();                            // Main reasoning loop
  bool TryTransition(Event event);           // Validate + execute transition
  void HandleThinking(const Observation& obs); // Build context, call LLM
  void HandleRouting(ActionCandidate ac);    // Route LLM output
  void HandleActing(ActionCandidate ac);     // Emit action frame (non-blocking)
  void HandleActingResult(const Observation& obs); // Process tool result
  void HandleResponding(ActionCandidate ac); // Deliver response
  bool CheckBudget();                        // Enforce guardrails
  void ResetBudgetWindow();                  // Re-arm counters after Idle
  void HandleInterrupt();                    // Cancel in-progress work
  void EmitDiagnostic(const std::string& message); // Notify diagnostic callbacks
  void EmitActivity(ActivityKind kind, std::string detail = {}); // Notify activity callbacks

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
  std::unique_ptr<ObservationAggregator> observation_aggregator_;
  std::unique_ptr<ObservationFilter> observation_filter_;
  std::unique_ptr<TtsSegmentStrategy> tts_segment_;
  std::unique_ptr<ResponseFilter> response_filter_;

  // Pending tool call state (set in HandleActing, read in HandleActingResult).
  // Supports parallel tool calls: pending_tool_calls_ tracks all outstanding
  // calls, pending_results_ collects results as they arrive.
  ActionCandidate pending_action_;
  std::vector<ToolCall> pending_tool_calls_;
  std::unordered_map<std::string, std::string> pending_results_;  // id → result JSON
  std::chrono::steady_clock::time_point tool_call_start_;         // for timeout

  // State (accessed from loop thread; read via atomic for external queries)
  std::atomic<State> state_{State::kIdle};

  // Observation queue (cross-thread)
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Observation> observation_queue_;

  // Session counters
  int turn_count_ = 0;
  int total_prompt_tokens_ = 0;
  int total_completion_tokens_ = 0;
  int action_count_ = 0;
  bool first_token_logged_ = false;
  std::chrono::steady_clock::time_point session_start_;
  std::chrono::steady_clock::time_point last_activity_;
  bool conversation_active_ = false;

  // Loop thread
  std::thread loop_thread_;
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> interrupt_requested_{false};

  // Callbacks (must be registered before Start())
  std::mutex callbacks_mutex_;
  std::vector<TransitionCallback> transition_callbacks_;
  std::vector<DiagnosticCallback> diagnostic_callbacks_;
  std::vector<ResponseCallback> response_callbacks_;
  std::vector<StreamTokenCallback> stream_token_callbacks_;
  std::vector<ActivityCallback> activity_callbacks_;
};

}  // namespace shizuru::core
