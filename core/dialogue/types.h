#pragma once

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "controller/types.h"   // Observation, ActivityKind

namespace shizuru::core::dialogue {

// ---------------------------------------------------------------------------
// DialogueEvent — Phase 1 subset
// ---------------------------------------------------------------------------

// Events handled by the Phase 1 reducer:

struct InterruptRequested {
  std::chrono::steady_clock::time_point now;
};

struct DebounceCooldownExpired {
  std::chrono::steady_clock::time_point now;
};

struct UserMessageReceived {
  Observation observation;
  std::chrono::steady_clock::time_point now;
};

struct ShutdownRequested {};

// Forward-compatibility stubs — declared but not handled by the reducer yet:

struct LlmCompleted {};
struct LlmFailed {};
struct ToolResultReceived {};
struct ToolCallTimeout {};
struct ContinuationRequested {};
struct SystemEventReceived {};

using DialogueEvent = std::variant<
    InterruptRequested,
    DebounceCooldownExpired,
    UserMessageReceived,
    ShutdownRequested,
    LlmCompleted,
    LlmFailed,
    ToolResultReceived,
    ToolCallTimeout,
    ContinuationRequested,
    SystemEventReceived>;

// ---------------------------------------------------------------------------
// CooldownPhase
// ---------------------------------------------------------------------------

enum class CooldownPhase {
  kNone,       // Normal operation.
  kDebouncing, // Post-interrupt buffering period.
};

// ---------------------------------------------------------------------------
// DialogueState — Phase 1 subset
// ---------------------------------------------------------------------------

struct DialogueState {
  bool conversation_active = false;

  CooldownPhase cooldown = CooldownPhase::kNone;

  int turn_count = 0;
  int total_prompt_tokens = 0;
  int total_completion_tokens = 0;
  int action_count = 0;

  std::chrono::steady_clock::time_point session_start;
  std::chrono::steady_clock::time_point last_activity;
};

// ---------------------------------------------------------------------------
// DialogueEffect — Phase 1 subset
// ---------------------------------------------------------------------------

struct RecordMemory {
  Observation observation;
};

struct CancelLlm {};

struct StartLlmContinuation {
  std::chrono::steady_clock::time_point now;
};

struct EmitActivityEffect {
  ActivityKind kind;
  std::string detail;
};

struct SignalBudgetExhausted {};

struct NoOp {};

using DialogueEffect = std::variant<
    RecordMemory,
    CancelLlm,
    StartLlmContinuation,
    EmitActivityEffect,
    SignalBudgetExhausted,
    NoOp>;

// ---------------------------------------------------------------------------
// DialogueDecision
// ---------------------------------------------------------------------------

struct DialogueDecision {
  DialogueState next_state;
  std::vector<DialogueEffect> effects;
};

}  // namespace shizuru::core::dialogue
