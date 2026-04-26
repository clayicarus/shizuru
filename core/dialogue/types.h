#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "context/types.h"      // MemoryEntryType
#include "controller/types.h"   // Observation, ActionCandidate, Event, ActivityKind

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

struct UserMessageReceived {
  Observation observation;
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

struct ToolResultReceived {
  Observation observation;
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

struct SystemEventReceived {
  Observation observation;
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

// Phase 3 new events:

struct UserFragmentReceived {
  Observation observation;
  std::chrono::steady_clock::time_point now;
};

struct AggregationComplete {
  Observation observation;
  std::chrono::steady_clock::time_point now;
};

struct AggregationTimeout {
  Observation observation;
  std::chrono::steady_clock::time_point now;
};

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
    SystemEventReceived,
    TimerExpired,
    TurnTriggerClassified,
    UserFragmentReceived,
    AggregationComplete,
    AggregationTimeout>;

// ---------------------------------------------------------------------------
// CooldownPhase
// ---------------------------------------------------------------------------

enum class CooldownPhase {
  kNone,       // Normal operation.
  kDebouncing, // Post-interrupt buffering period.
};

// ---------------------------------------------------------------------------
// Workspace Types — Phase 3
// ---------------------------------------------------------------------------

// A single entry in the provisional turn workspace.
struct WorkspaceEntry {
  std::string content;
  std::string source;
  MemoryEntryType entry_type = MemoryEntryType::kUserMessage;
  std::chrono::steady_clock::time_point timestamp;
};

// Provisional turn workspace — holds data that has not yet been committed
// to ContextStrategy.  Entries are promoted via CommitWorkspace effect.
struct TurnWorkspace {
  // Buffered user input fragments (accumulated during debounce or
  // aggregation).  Merged into a single MemoryEntry on CommitWorkspace
  // when merge_fragments is true.
  std::vector<WorkspaceEntry> user_fragments;

  // In-progress assistant output.  Set when the reducer knows an assistant
  // response is being prepared but not yet committed.  Cleared on commit
  // or discard.
  std::optional<WorkspaceEntry> assistant_partial;
};

// ---------------------------------------------------------------------------
// DialogueState — Phase 1 + Phase 2 + Phase 3
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

  // --- Phase 3 addition ---
  TurnWorkspace workspace;
};

// ---------------------------------------------------------------------------
// DialogueEffect — Phase 1 + Phase 2
// ---------------------------------------------------------------------------

// Phase 1 effects:

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

// Phase 2 new effects:

struct StartLlm {
  Observation trigger;
};

struct EmitToolCallFrames {
  ActionCandidate action;
};

struct RecordToolResult {
  Observation observation;
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

struct StartTurnTriggerClassification {
  uint64_t obs_id;
  Observation observation;
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

// Phase 3 new effects:

struct BufferToWorkspace {
  WorkspaceEntry entry;
  bool is_fragment;  // true = user fragment, will be merged on commit
};

struct CommitWorkspace {
  bool merge_fragments;  // true = merge user_fragments into single entry
};

struct DiscardWorkspace {};

using DialogueEffect = std::variant<
    RecordMemory,
    CancelLlm,
    StartLlmContinuation,
    EmitActivityEffect,
    SignalBudgetExhausted,
    NoOp,
    StartLlm,
    EmitToolCallFrames,
    RecordToolResult,
    RecordToolCallDecision,
    DeliverResponse,
    ResetBudgetWindow,
    EmitDiagnosticEffect,
    ScheduleTimer,
    CancelTimer,
    StartTurnTriggerClassification,
    CancelTurnTriggerClassification,
    TransitionState,
    RecordInterruptMemory,
    RecordTimeoutResults,
    BufferToWorkspace,
    CommitWorkspace,
    DiscardWorkspace>;

// ---------------------------------------------------------------------------
// DialogueDecision
// ---------------------------------------------------------------------------

struct DialogueDecision {
  DialogueState next_state;
  std::vector<DialogueEffect> effects;
};

}  // namespace shizuru::core::dialogue
