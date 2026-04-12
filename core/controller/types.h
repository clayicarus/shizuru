#pragma once

#include <chrono>
#include <string>

namespace shizuru::core {

enum class State {
  kIdle,
  kListening,
  kThinking,
  kRouting,
  kActing,
  kResponding,
  kError,
  kTerminated,
};

enum class Event {
  kStart,
  kStop,
  kShutdown,
  kUserObservation,
  kLlmResult,
  kLlmFailure,
  kRouteToAction,
  kRouteToResponse,
  kRouteToContinue,
  kActionComplete,
  kActionFailed,
  kResponseDelivered,
  kStopConditionMet,
  kInterrupt,
  kRecover,
};

enum class ActionType {
  kToolCall,
  kResponse,
  kContinue,
};

enum class ObservationType {
  kUserMessage,
  kToolResult,
  kSystemEvent,
  kInterruption,
  kContinuation,  // Signals next thinking step; no message appended to context.
};

// An input event from the external environment.
struct Observation {
  ObservationType type;
  std::string content;   // Serialized payload
  std::string source;    // Origin identifier (e.g., "user", "tool:web_search")
  std::chrono::steady_clock::time_point timestamp;
};

// A single tool call proposed by the LLM.
struct ToolCall {
  std::string id;                   // Tool call ID for pairing with results
  std::string name;                 // Tool name
  std::string arguments;            // Serialized arguments (JSON string)
  std::string required_capability;  // Capability needed to execute
};

// An action proposed by the LLM.
struct ActionCandidate {
  ActionType type;
  std::string action_name;          // Tool name (for single kToolCall, legacy)
  std::string arguments;            // Serialized arguments (for single kToolCall, legacy)
  std::string response_text;        // Response content (for kResponse)
  std::string required_capability;  // Capability needed to execute

  // Parallel tool calls — populated when LLM returns multiple tool_calls.
  // For single tool call, this has one entry and action_name/arguments mirror it.
  std::vector<ToolCall> tool_calls;
};

// Human-readable name helpers — used by logging and audit formatting.
inline const char* StateName(State s) {
  switch (s) {
    case State::kIdle:        return "Idle";
    case State::kListening:   return "Listening";
    case State::kThinking:    return "Thinking";
    case State::kRouting:     return "Routing";
    case State::kActing:      return "Acting";
    case State::kResponding:  return "Responding";
    case State::kError:       return "Error";
    case State::kTerminated:  return "Terminated";
    default:                  return "Unknown";
  }
}

inline const char* EventName(Event e) {
  switch (e) {
    case Event::kStart:              return "Start";
    case Event::kStop:               return "Stop";
    case Event::kShutdown:           return "Shutdown";
    case Event::kUserObservation:    return "UserObservation";
    case Event::kLlmResult:          return "LlmResult";
    case Event::kLlmFailure:         return "LlmFailure";
    case Event::kRouteToAction:      return "RouteToAction";
    case Event::kRouteToResponse:    return "RouteToResponse";
    case Event::kRouteToContinue:    return "RouteToContinue";
    case Event::kActionComplete:     return "ActionComplete";
    case Event::kActionFailed:       return "ActionFailed";
    case Event::kResponseDelivered:  return "ResponseDelivered";
    case Event::kStopConditionMet:   return "StopConditionMet";
    case Event::kInterrupt:          return "Interrupt";
    case Event::kRecover:            return "Recover";
    default:                         return "Unknown";
  }
}

// Structured activity events for UI consumption.
// Unlike DiagnosticCallback (free-form debug text), these are a stable API
// contract between the controller and the presentation layer.
enum class ActivityKind {
  kBufferingInput,      // 0  Aggregator is buffering ASR fragments
  kFilteringInput,      // 1  Observation filter is evaluating relevance
  kThinkingStarted,     // 2  LLM request submitted
  kThinkingRetry,       // 3  LLM request retrying after transient failure
  kToolDispatched,      // 4  Tool call emitted, waiting for result
  kToolResultReceived,  // 5  Tool result arrived
  kSpeaking,            // 6  Response / TTS segment being delivered
  kInterrupted,         // 7  Turn interrupted by user
  kTurnComplete,        // 8  Turn finished, back to listening
  kBudgetExhausted,     // 9  Budget limit hit, entering idle
};

struct ActivityEvent {
  ActivityKind kind;
  std::string detail;  // Optional: tool name, retry count, filter reason, etc.
};

}  // namespace shizuru::core
