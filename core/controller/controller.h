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
#include "interfaces/io_bridge.h"
#include "interfaces/llm_client.h"
#include "policy/policy_layer.h"

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
  // All dependencies injected via constructor.
  // session_id must match the key used to initialize ContextStrategy and
  // PolicyLayer (via InitSession), so all lookups resolve to the same slot.
  Controller(std::string session_id,
             ControllerConfig config,
             std::unique_ptr<LlmClient> llm,
             std::unique_ptr<IoBridge> io,
             ContextStrategy& context,
             PolicyLayer& policy);

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

  // Register callback for diagnostic events.
  using DiagnosticCallback = std::function<void(const std::string& message)>;
  void OnDiagnostic(DiagnosticCallback cb);

  // Register callback for assistant text responses.
  using ResponseCallback = std::function<void(const ActionCandidate& response)>;
  void OnResponse(ResponseCallback cb);

 private:
  static constexpr char MODULE_NAME[] = "Controller";

  std::string session_id_;

  void RunLoop();                            // Main reasoning loop
  bool TryTransition(Event event);           // Validate + execute transition
  void HandleThinking(const Observation& obs); // Build context, call LLM
  void HandleRouting(ActionCandidate ac);    // Route LLM output
  void HandleActing(ActionCandidate ac);     // Execute IO action
  void HandleResponding(ActionCandidate ac); // Deliver response
  bool CheckBudget();                        // Enforce guardrails
  void HandleInterrupt();                    // Cancel in-progress work
  void EmitDiagnostic(const std::string& message); // Notify diagnostic callbacks

  // Static transition table
  static const std::unordered_map<std::pair<State, Event>, State, PairHash>
      kTransitionTable;

  ControllerConfig config_;
  std::unique_ptr<LlmClient> llm_;
  std::unique_ptr<IoBridge> io_;
  ContextStrategy& context_;
  PolicyLayer& policy_;

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
  std::chrono::steady_clock::time_point session_start_;

  // Loop thread
  std::thread loop_thread_;
  std::atomic<bool> shutdown_requested_{false};

  // Callbacks
  std::vector<TransitionCallback> transition_callbacks_;
  std::vector<DiagnosticCallback> diagnostic_callbacks_;
  std::vector<ResponseCallback> response_callbacks_;
};

}  // namespace shizuru::core
