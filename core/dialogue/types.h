#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "controller/types.h"       // ActionCandidate, Event, ActivityKind
#include "core/conversation_item.h" // ConversationItem

namespace shizuru::core::dialogue {

// ---------------------------------------------------------------------------
// Phase 2 enums
// ---------------------------------------------------------------------------

enum class TimerKind {
  kDebounce,
  kToolCallTimeout,
  kConversationIdle,
  kAggregationTimeout,  // Phase 3: aggregator timeout
};

enum class TurnTriggerVerdict {
  kRespondNow,
  kStoreOnly,
};

enum class DeliberationPhase {
  kIdle,
  kAwaitingTurnTrigger,
  kThinking,
  kAwaitingToolResults,
};

// ---------------------------------------------------------------------------
// DialogueEvent — Phase 1 + Phase 2
// ---------------------------------------------------------------------------

// Phase 1 events:

struct InterruptRequested {
  std::chrono::steady_clock::time_point now;
};

struct DebounceCooldownExpired {
  std::chrono::steady_clock::time_point now;
};

struct ShutdownRequested {};

// Phase 1 stubs — now populated with real payloads (Phase 2):

struct LlmCompleted {
  ActionCandidate candidate;
  int prompt_tokens = 0;
  int completion_tokens = 0;
  std::chrono::steady_clock::time_point now;
};

struct LlmFailed {
  std::string reason;
  std::chrono::steady_clock::time_point now;
};

struct ToolCallTimeout {
  std::vector<std::string> missing_tool_call_ids;
  std::chrono::steady_clock::time_point now;
};

struct ContinuationRequested {
  std::string source;
  std::chrono::steady_clock::time_point now;
};

// Phase 2 new events:

struct TimerExpired {
  TimerKind kind;
  std::string timer_id;
  std::chrono::steady_clock::time_point now;
};

struct TurnTriggerClassified {
  uint64_t obs_id;
  TurnTriggerVerdict verdict;
  std::chrono::steady_clock::time_point now;
};

// Phase 4 events — ConversationItem-based (replacing old Observation events):

struct ConversationItemReceived {
  ConversationItem item;
  std::chrono::steady_clock::time_point now;
};

struct ToolResultReceived {
  ConversationItem item;  // kind = kToolResult
  std::chrono::steady_clock::time_point now;
};

struct SystemEventReceived {
  ConversationItem item;  // kind = kSystemEvent
  std::chrono::steady_clock::time_point now;
};

using DialogueEvent = std::variant<
    InterruptRequested,
    DebounceCooldownExpired,
    ShutdownRequested,
    LlmCompleted,
    LlmFailed,
    ToolCallTimeout,
    ContinuationRequested,
    TimerExpired,
    TurnTriggerClassified,
    ConversationItemReceived,
    ToolResultReceived,
    SystemEventReceived>;

// ---------------------------------------------------------------------------
// CooldownPhase
// ---------------------------------------------------------------------------

enum class CooldownPhase {
  kNone,       // Normal operation.
  kDebouncing, // Post-interrupt buffering period.
};

// ---------------------------------------------------------------------------
// DialogueState — Phase 1 + Phase 2
// ---------------------------------------------------------------------------

struct DialogueState {
  // --- Session-level fields ---
  bool conversation_active = false;

  CooldownPhase cooldown = CooldownPhase::kNone;

  std::chrono::steady_clock::time_point session_start;
  std::chrono::steady_clock::time_point last_activity;

  // --- Per-turn counters (reset when a new user turn starts) ---
  int turn_llm_calls = 0;          // LLM calls in current turn
  int turn_prompt_tokens = 0;      // Prompt tokens in current turn
  int turn_completion_tokens = 0;  // Completion tokens in current turn
  int turn_action_count = 0;       // Tool calls in current turn
  int turn_continuation_count = 0; // kContinue responses in current turn

  // --- Phase 2 additions ---
  DeliberationPhase deliberation = DeliberationPhase::kIdle;

  // Monotonic counter for generating unique turn-trigger correlation ids.
  // Only incremented, never reset — ensures ids are never reused even after
  // interrupt resets pending_turn_trigger_id to 0.
  uint64_t next_turn_trigger_id = 0;

  // The id of the currently in-flight turn-trigger evaluation.
  // Set to a value from next_turn_trigger_id when classification starts.
  // Reset to 0 on interrupt or when classification completes.
  // Stale TurnTriggerClassified events are rejected by
  // obs_id != pending_turn_trigger_id.
  uint64_t pending_turn_trigger_id = 0;

  // Pending tool calls (ids of tool calls awaiting results).
  std::vector<std::string> pending_tool_call_ids;

  // Collected tool results (id → result JSON).
  std::unordered_map<std::string, std::string> pending_tool_results;
};

// ---------------------------------------------------------------------------
// DialogueEffect — Phase 1 + Phase 2
// ---------------------------------------------------------------------------

// Phase 1 effects:

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

// Phase 2 new effects:

struct EmitToolCallFrames {
  ActionCandidate action;
};

struct RecordToolCallDecision {
  ActionCandidate action;
};

struct DeliverResponse {
  ActionCandidate action;
};

struct ResetBudgetWindow {};

struct EmitDiagnosticEffect {
  std::string message;
};

struct ScheduleTimer {
  TimerKind kind;
  std::string timer_id;
  std::chrono::steady_clock::time_point deadline;
};

struct CancelTimer {
  std::string timer_id;
};

struct CancelTurnTriggerClassification {
  uint64_t obs_id;
};

struct TransitionState {
  Event event;
};

struct RecordInterruptMemory {};

struct RecordTimeoutResults {
  std::vector<std::string> missing_tool_call_ids;
};

// Phase 4 effects — ConversationItem-based (replacing old Observation effects):

struct RecordConversationItem {
  ConversationItem item;
};

struct StartLlmWithBatch {
  std::vector<ConversationItem> items;
};

struct RecordToolResultItem {
  ConversationItem item;  // kind = kToolResult
};

using DialogueEffect = std::variant<
    CancelLlm,
    StartLlmContinuation,
    EmitActivityEffect,
    SignalBudgetExhausted,
    NoOp,
    EmitToolCallFrames,
    RecordToolCallDecision,
    DeliverResponse,
    ResetBudgetWindow,
    EmitDiagnosticEffect,
    ScheduleTimer,
    CancelTimer,
    CancelTurnTriggerClassification,
    TransitionState,
    RecordInterruptMemory,
    RecordTimeoutResults,
    RecordConversationItem,
    StartLlmWithBatch,
    RecordToolResultItem>;

// ---------------------------------------------------------------------------
// DialogueDecision
// ---------------------------------------------------------------------------

struct DialogueDecision {
  DialogueState next_state;
  std::vector<DialogueEffect> effects;
};

}  // namespace shizuru::core::dialogue
