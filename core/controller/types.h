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

// An action proposed by the LLM.
struct ActionCandidate {
  ActionType type;
  std::string action_name;          // Tool name (for kToolCall)
  std::string arguments;            // Serialized arguments (JSON string)
  std::string response_text;        // Response content (for kResponse)
  std::string required_capability;  // Capability needed to execute
};

}  // namespace shizuru::core
