# Requirements Document

## Introduction

Phase 2 of the dialogue kernel migrates the remaining post-aggregation
turn-taking semantics from `Controller` inline logic into the dialogue
reducer. Phase 1 proved the pattern by moving barge-in and debounce into the
reducer. Phase 2 continues by routing normal user messages, LLM
completion/failure, tool result/timeout, system events, and continuation
requests through the reducer. It also introduces a `TimerBook` for explicit
timer management, adds `DeliberationPhase` tracking, and replaces the
reducer-owned "filter" concept with an async turn-trigger classification
effect. After Phase 2, `Controller::RunLoop` becomes a thin event-mapping and
effect-execution shell for all post-aggregation paths, and the reducer is the
single source of truth for dialogue state and budget counters.

**Scope boundary:** `ObservationAggregator` remains a synchronous
pre-reducer step in `Controller`. Aggregator event-ization is deferred to
Phase 3. Data-plane noise filtering is also out of scope for Phase 2: VAD
noise rejection, ASR garbage suppression, transcript sanitization, and other
input-validity checks belong upstream of `core/`.

**Behavioral continuity:** Once a user observation reaches `core/`, it is
treated as meaningful and worth preserving in committed history. The new
semantic classifier decides only whether the observation should trigger an
assistant turn immediately (`kRespondNow`) or be stored without response
(`kStoreOnly`).

## Glossary

- **Reducer**: A pure function that takes `(DialogueState, DialogueEvent)` and
  returns a `DialogueDecision` containing `next_state` and a list of effects.
  It performs no I/O, no blocking, and no mutation of shared state.
- **DialogueState**: Explicit struct capturing conversation-active flag,
  cooldown phase, deliberation phase, budget counters, pending turn-trigger
  correlation id, pending tool call ids, and pending tool results.
- **DialogueEvent**: A `std::variant` representing the full event taxonomy
  including all Phase 1 events plus `TimerExpired`, `TurnTriggerClassified`,
  and populated payloads for `LlmCompleted`, `LlmFailed`,
  `ToolResultReceived`, `ToolCallTimeout`, `ContinuationRequested`, and
  `SystemEventReceived`.
- **DialogueEffect**: A `std::variant` representing side-effect instructions
  produced by the reducer. Phase 2 adds `StartLlm`, `EmitToolCallFrames`,
  `RecordToolResult`, `RecordToolCallDecision`, `DeliverResponse`,
  `ResetBudgetWindow`, `EmitDiagnosticEffect`, `ScheduleTimer`, `CancelTimer`,
  `StartTurnTriggerClassification`, `CancelTurnTriggerClassification`,
  `TransitionState`, `RecordInterruptMemory`, and `RecordTimeoutResults`.
- **DialogueDecision**: A struct containing `next_state` (`DialogueState`) and
  `effects` (`std::vector<DialogueEffect>`).
- **DeliberationPhase**: An enum tracking reducer-owned deliberation state:
  `kIdle`, `kAwaitingTurnTrigger`, `kThinking`, `kAwaitingToolResults`.
- **TimerBook**: A generation-safe timer data structure owned by Controller.
  The reducer produces `ScheduleTimer` / `CancelTimer` effects; Controller
  uses `TimerBook::NextDeadline()` to compute wait durations and
  `TimerBook::PopExpired(now)` to harvest `TimerExpired` events.
- **TimerKind**: An enum identifying timer purpose: `kDebounce`,
  `kToolCallTimeout`, `kConversationIdle`.
- **TurnTriggerVerdict**: An enum with values `kRespondNow` and `kStoreOnly`.
- **TurnTriggerClassifier**: The core-level semantic worker that decides
  whether an already meaningful observation should start an assistant turn
  immediately. It does not decide whether the observation is valid data.
- **ObservationFilter**: Legacy name of the existing strategy interface.
  During migration it may be adapted underneath `TurnTriggerClassifier`, but
  it no longer represents data-plane noise filtering in the `core/` design.
- **InputSanitizer**: Any upstream data-plane component that removes invalid
  input before it reaches `core/` (for example VAD/ASR garbage suppression or
  transcript cleanup). Not part of Phase 2.
- **Controller**: The event-loop shell in `core/controller/`. After Phase 2,
  it maps runtime inputs to `DialogueEvent`s, calls the reducer, and executes
  effects. It no longer owns semantic turn-taking decisions.
- **Effect Executor**: The part of Controller that iterates over
  `DialogueDecision.effects` and performs the corresponding I/O or state
  transitions. Implemented as `Controller::ApplyDialogueDecision`.

## Requirements

### Requirement 1: Expanded Event Taxonomy

**User Story:** As a core developer, I want the dialogue event variant to
cover all Controller branches with real payloads, so that every Controller
code path maps to a typed event.

#### Acceptance Criteria

1. THE Dialogue_Module SHALL populate `LlmCompleted` with `candidate`,
   `prompt_tokens`, `completion_tokens`, and `now`
2. THE Dialogue_Module SHALL populate `LlmFailed` with `reason` and `now`
3. THE Dialogue_Module SHALL populate `ToolResultReceived` with `observation`
   and `now`
4. THE Dialogue_Module SHALL populate `ToolCallTimeout` with
   `missing_tool_call_ids` and `now`
5. THE Dialogue_Module SHALL populate `ContinuationRequested` with `source`
   and `now`
6. THE Dialogue_Module SHALL populate `SystemEventReceived` with `observation`
   and `now`
7. THE Dialogue_Module SHALL add `TimerExpired` with `kind`, `timer_id`, and
   `now`
8. THE Dialogue_Module SHALL add `TurnTriggerClassified` with `obs_id`,
   `verdict`, and `now`
9. THE Dialogue_Module SHALL define `TimerKind` with values `kDebounce`,
   `kToolCallTimeout`, and `kConversationIdle`
10. THE Dialogue_Module SHALL define `TurnTriggerVerdict` with values
    `kRespondNow` and `kStoreOnly`
11. THE Dialogue_Module SHALL update `DialogueEvent` to include
    `TimerExpired` and `TurnTriggerClassified`

### Requirement 2: Expanded Dialogue State

**User Story:** As a core developer, I want the dialogue state to track
deliberation phase, pending turn-trigger work, and pending tool calls, so
that the reducer can reason about LLM, tool, and semantic decision lifecycle.

#### Acceptance Criteria

1. THE Dialogue_Module SHALL define `DeliberationPhase` with values `kIdle`,
   `kAwaitingTurnTrigger`, `kThinking`, and `kAwaitingToolResults`
2. THE Dialogue_Module SHALL add `deliberation` to `DialogueState`,
   initialized to `kIdle`
3. THE Dialogue_Module SHALL add `pending_turn_trigger_id` (`uint64_t`) to
   `DialogueState`, initialized to `0`
4. THE Dialogue_Module SHALL add `next_turn_trigger_id` (`uint64_t`) to
   `DialogueState`, initialized to `0` — this is a monotonic counter that is
   only incremented, never reset, ensuring unique correlation ids even after
   interrupts
5. THE Dialogue_Module SHALL add `pending_tool_call_ids`
   (`std::vector<std::string>`) to `DialogueState`
6. THE Dialogue_Module SHALL add `pending_tool_results`
   (`std::unordered_map<std::string, std::string>`) to `DialogueState`
7. WHILE `deliberation == kAwaitingTurnTrigger`, THE DialogueState SHALL have
   `pending_turn_trigger_id > 0`
8. WHILE `deliberation == kAwaitingToolResults`, THE DialogueState SHALL have
   non-empty `pending_tool_call_ids`

### Requirement 3: Expanded Effect Vocabulary

**User Story:** As a core developer, I want the effect variant to cover all
side effects needed by the newly reducer-owned paths, so that the effect
executor can handle LLM starts, tool dispatch, timer management, semantic
classification, and timeout recording.

#### Acceptance Criteria

1. THE Dialogue_Module SHALL add `StartLlm{trigger}`
2. THE Dialogue_Module SHALL add `EmitToolCallFrames{action}`
3. THE Dialogue_Module SHALL add `RecordToolResult{observation}`
4. THE Dialogue_Module SHALL add `RecordToolCallDecision{action}`
5. THE Dialogue_Module SHALL add `DeliverResponse{action}`
6. THE Dialogue_Module SHALL add `ResetBudgetWindow{}`
7. THE Dialogue_Module SHALL add `EmitDiagnosticEffect{message}`
8. THE Dialogue_Module SHALL add `ScheduleTimer{kind, timer_id, deadline}`
9. THE Dialogue_Module SHALL add `CancelTimer{timer_id}`
10. THE Dialogue_Module SHALL add
    `StartTurnTriggerClassification{obs_id, observation}`
11. THE Dialogue_Module SHALL add
    `CancelTurnTriggerClassification{obs_id}`
12. THE Dialogue_Module SHALL add `TransitionState{event}`
13. THE Dialogue_Module SHALL add `RecordInterruptMemory{}`
14. THE Dialogue_Module SHALL add
    `RecordTimeoutResults{missing_tool_call_ids}`
15. THE Dialogue_Module SHALL update `DialogueEffect` to include all new
    effect types

### Requirement 4: TimerBook Data Structure

**User Story:** As a core developer, I want a generation-safe timer data
structure, so that Controller can replace hardcoded `wait_for` durations with
explicit timer scheduling driven by reducer effects without timer-id reuse
bugs.

#### Acceptance Criteria

1. THE TimerBook SHALL provide `Schedule(kind, timer_id, deadline)` and
   increment the generation for that `timer_id`, so any previous in-heap entry
   with the same id becomes stale
2. THE TimerBook SHALL provide `Cancel(timer_id)` and increment the generation
   for that id without pushing a new entry
3. THE TimerBook SHALL provide `NextDeadline()` and return the earliest
   non-stale deadline, or `nullopt` if no active timers remain
4. THE TimerBook SHALL provide `PopExpired(now)` and return all non-stale
   entries whose deadline is at or before `now`, in deadline order
5. THE TimerBook SHALL skip stale entries during `NextDeadline()` and
   `PopExpired(now)` without returning them
6. THE TimerBook SHALL provide `Empty()` returning `true` when no non-stale
   timers remain
7. THE TimerBook SHALL reside in `core/dialogue/timer_book.h` and depend only
   on C++17 standard library headers
8. THE TimerBook SHALL be generation-safe for the sequence
   `Schedule(id) -> Cancel(id) -> Schedule(id)`

### Requirement 5: Normal User Message Through Reducer

**User Story:** As a core developer, I want normal user messages
(non-barge-in) routed through the reducer, so that the standard message path
is expressed as explicit state transitions and effects.

#### Acceptance Criteria

1. WHEN a `UserMessageReceived` event is received and `cooldown == kNone` and
   `deliberation == kIdle`, THE DefaultDialogueReducer SHALL include both
   `RecordMemory` and `StartTurnTriggerClassification` in the effects list and
   set `deliberation = kAwaitingTurnTrigger`
2. WHEN a `UserMessageReceived` event is received and the conversation has
   been idle longer than `conversation_idle_timeout`, THE
   DefaultDialogueReducer SHALL include `ResetBudgetWindow` in the effects
   list and reset budget counters in `next_state`
3. WHEN a `UserMessageReceived` event is received, THE DefaultDialogueReducer
   SHALL set `next_state.conversation_active = true` and update
   `next_state.last_activity` to `event.now`

### Requirement 6: TurnTriggerClassified Handling

**User Story:** As a core developer, I want the reducer to handle turn-trigger
results, so that async semantic verdicts drive either immediate thinking or
store-only behavior through explicit state transitions.

#### Acceptance Criteria

1. WHEN a `TurnTriggerClassified` event is received with verdict
   `kRespondNow` and `obs_id == pending_turn_trigger_id`, THE
   DefaultDialogueReducer SHALL set `deliberation = kThinking` and include
   `StartLlm` in the effects list
2. WHEN a `TurnTriggerClassified` event is received with verdict
   `kStoreOnly` and `obs_id == pending_turn_trigger_id`, THE
   DefaultDialogueReducer SHALL set `deliberation = kIdle` and return an empty
   effects list; the message remains recorded in committed history
3. WHEN a `TurnTriggerClassified` event is received with an `obs_id` that
   does not match `pending_turn_trigger_id`, THE DefaultDialogueReducer SHALL
   return the current state unchanged with no effects

### Requirement 7: LLM Completion Handling

**User Story:** As a core developer, I want the reducer to handle LLM
completion events, so that token accounting, turn counting, and action
routing are expressed as pure state transitions.

#### Acceptance Criteria

1. WHEN an `LlmCompleted` event is received, THE DefaultDialogueReducer SHALL
   increment `next_state.turn_count`
2. WHEN an `LlmCompleted` event is received, THE DefaultDialogueReducer SHALL
   accumulate prompt and completion tokens into `next_state`
3. WHEN an `LlmCompleted` event is received with a `candidate` of type
   `kToolCall`, THE DefaultDialogueReducer SHALL set
   `deliberation = kAwaitingToolResults`, populate `pending_tool_call_ids`,
   and include `RecordToolCallDecision` and `EmitToolCallFrames`
4. WHEN an `LlmCompleted` event is received with a `candidate` of type
   `kResponse`, THE DefaultDialogueReducer SHALL set `deliberation = kIdle`
   and include `DeliverResponse`
5. WHEN an `LlmCompleted` event is received with a `candidate` of type
   `kContinue`, THE DefaultDialogueReducer SHALL keep
   `deliberation = kThinking` and include `StartLlm`
6. WHEN an `LlmCompleted` event is received, THE DefaultDialogueReducer SHALL
   update `next_state.last_activity` to `event.now`

### Requirement 8: LLM Failure Handling

**User Story:** As a core developer, I want the reducer to handle LLM failure
events, so that error recovery is expressed as an explicit state transition.

#### Acceptance Criteria

1. WHEN an `LlmFailed` event is received, THE DefaultDialogueReducer SHALL set
   `deliberation = kIdle` and include `EmitDiagnosticEffect`
2. WHEN an `LlmFailed` event is received, THE DefaultDialogueReducer SHALL
   include `TransitionState{Event::kLlmFailure}`

### Requirement 9: Tool Result Handling

**User Story:** As a core developer, I want the reducer to handle tool result
events, so that result collection and continuation logic are expressed as pure
state transitions.

#### Acceptance Criteria

1. WHEN a `ToolResultReceived` event is received, THE DefaultDialogueReducer
   SHALL record the result in `next_state.pending_tool_results`
2. WHEN all pending tool calls have results, THE DefaultDialogueReducer SHALL
   set `deliberation = kThinking`, clear pending collections, and include
   `RecordToolResult` and `StartLlm`
3. WHEN some pending tool calls still lack results, THE
   DefaultDialogueReducer SHALL keep `deliberation = kAwaitingToolResults` and
   include `RecordToolResult`
4. WHEN a `ToolResultReceived` event is received, THE DefaultDialogueReducer
   SHALL update `next_state.last_activity` to `event.now`

### Requirement 10: Tool Call Timeout Handling

**User Story:** As a core developer, I want the reducer to handle tool call
timeout events, so that timeout recovery is expressed as an explicit state
transition and timeout information is visible to the LLM on continuation.

#### Acceptance Criteria

1. WHEN a `ToolCallTimeout` event is received, THE DefaultDialogueReducer
   SHALL include `RecordTimeoutResults{missing_tool_call_ids}` in the effects
   list — the reducer does NOT write timeout entries into
   `pending_tool_results`; that is the effect executor's responsibility
2. THE effect executor SHALL record synthetic timeout error entries to
   committed history for each missing tool call id before continuation LLM
   thinking begins
3. WHEN a `ToolCallTimeout` event is received, THE DefaultDialogueReducer
   SHALL set `deliberation = kThinking`, clear `pending_tool_call_ids` and
   `pending_tool_results`, and include `StartLlm`

### Requirement 11: Timer-Based Debounce

**User Story:** As a core developer, I want the reducer to produce explicit
timer effects for debounce instead of relying on implicit `wait_for` polling,
so that timer management is testable.

#### Acceptance Criteria

1. WHEN an `InterruptRequested` event is received, THE DefaultDialogueReducer
   SHALL include `ScheduleTimer` with `kind = kDebounce`
2. WHEN a `TimerExpired` event is received with `kind = kDebounce`, THE
   DefaultDialogueReducer SHALL handle it equivalently to
   `DebounceCooldownExpired`
3. THE DefaultDialogueReducer SHALL continue to handle
   `DebounceCooldownExpired` for backward compatibility during transition

### Requirement 12: System Event Handling

**User Story:** As a core developer, I want the reducer to handle system
events (scheduler reminders, followups), so that system event processing is
expressed through the reducer.

#### Acceptance Criteria

1. WHEN a `SystemEventReceived` event is received and `deliberation == kIdle`,
   THE DefaultDialogueReducer SHALL include `RecordMemory` and `StartLlm` and
   set `deliberation = kThinking`
2. WHEN a `SystemEventReceived` event is received, THE DefaultDialogueReducer
   SHALL set `next_state.conversation_active = true` and update
   `next_state.last_activity`

### Requirement 13: Continuation Request Handling

**User Story:** As a core developer, I want the reducer to handle
continuation requests, so that post-tool-result and post-denial continuation
thinking is expressed through the reducer.

#### Acceptance Criteria

1. WHEN a `ContinuationRequested` event is received and `deliberation` is
   `kIdle` or `kThinking`, THE DefaultDialogueReducer SHALL include `StartLlm`
   and set `deliberation = kThinking`

### Requirement 14: Interrupt During Turn-Trigger Evaluation

**User Story:** As a core developer, I want the reducer to cancel in-flight
turn-trigger evaluation when an interrupt arrives, so that stale semantic
results do not trigger thinking after a barge-in.

#### Acceptance Criteria

1. WHEN an `InterruptRequested` event is received and
   `deliberation == kAwaitingTurnTrigger`, THE DefaultDialogueReducer SHALL
   include `CancelTurnTriggerClassification` and reset
   `pending_turn_trigger_id` to `0`
2. WHEN an `InterruptRequested` event is received and
   `deliberation == kAwaitingToolResults`, THE DefaultDialogueReducer SHALL
   include `CancelTimer` for the tool timeout timer
3. WHEN an `InterruptRequested` event is received, THE DefaultDialogueReducer
   SHALL include `RecordInterruptMemory`

### Requirement 15: Controller as Thin Shell

**User Story:** As a core developer, I want Controller to delegate all
semantic decisions to the reducer after Phase 2, so that Controller becomes a
pure event-mapping and effect-execution shell.

#### Acceptance Criteria

1. THE Controller SHALL remove `SyncBudgetToDialogueState()` and use
   `dialogue_state_` as the single source of truth for budget counters
2. THE Controller SHALL remove redundant member fields after the reducer owns
   them (`turn_count_`, `total_prompt_tokens_`, `total_completion_tokens_`,
   `action_count_`, `conversation_active_`, `session_start_`,
   `last_activity_`, `pending_tool_calls_`, `pending_results_`)
3. THE Controller SHALL map every observation type and async completion to a
   `DialogueEvent` and call `Reduce` instead of inline semantic branching
4. THE Controller::ApplyDialogueDecision SHALL execute all new Phase 2 effects
   including `StartTurnTriggerClassification`,
   `CancelTurnTriggerClassification`, `RecordTimeoutResults`, and
   `RecordInterruptMemory`
5. THE Controller SHALL own a `TimerBook` instance and use
   `TimerBook::NextDeadline()` to compute `wait_for` durations in `RunLoop`
6. THE Controller SHALL call `TimerBook::PopExpired(now)` on each loop
   iteration and feed resulting `TimerExpired` events to the reducer

### Requirement 16: Reducer Purity

**User Story:** As a core developer, I want the reducer to remain a pure
function after Phase 2 expansion, so that all dialogue transitions are
deterministic and testable in isolation.

#### Acceptance Criteria

1. THE DefaultDialogueReducer::Reduce SHALL perform no I/O, no blocking, and
   no mutation of shared state
2. THE DefaultDialogueReducer::Reduce SHALL NOT call
   `std::chrono::steady_clock::now()` or any other system clock function
3. ALL time-dependent decisions in the reducer SHALL use timestamps carried by
   the input event structs
4. WHEN `Reduce` is called twice with identical `DialogueState` and
   `DialogueEvent` inputs, it SHALL return identical `DialogueDecision`
   outputs

### Requirement 17: Module Isolation

**User Story:** As a core developer, I want the dialogue module to maintain
zero dependencies on `io/`, `runtime/`, `services/`, or `app/`, so that it
remains testable in isolation.

#### Acceptance Criteria

1. THE Dialogue_Module SHALL NOT include headers from `io/`, `runtime/`,
   `services/`, or `app/`
2. THE Dialogue_Module SHALL NOT introduce new dependencies beyond C++17
   standard library headers and existing `core/` headers

### Requirement 18: Behavioral Equivalence

**User Story:** As a core developer, I want the Phase 2 reducer integration to
produce equivalent external behavior to the current Controller for all
meaningful observation sequences, so that the refactor is safe.

#### Acceptance Criteria

1. THE existing `controller_test.cpp`, `controller_prop_test.cpp`,
   `controller_bug_condition_test.cpp`, and
   `controller_preservation_test.cpp` SHALL pass without modification after
   Phase 2 integration
2. WHEN the Controller processes meaningful observation sequences through the
   reducer path, it SHALL produce the same LLM calls, memory recordings, tool
   dispatches, activity events, and state transitions as the current
   Controller, except where the new turn-trigger classifier intentionally
   returns `kStoreOnly`
3. THE test suite SHALL include controller-level regression tests covering:
   normal user message -> respond-now verdict -> think -> respond;
   normal user message -> store-only verdict -> no assistant turn;
   normal user message -> think -> tool call -> tool result -> continuation ->
   respond; LLM failure -> error recovery; tool call timeout -> timeout
   results recorded -> continuation; and system event -> think -> respond

### Requirement 19: Testing Infrastructure

**User Story:** As a core developer, I want dedicated unit and property-based
tests for all Phase 2 reducer handlers and the TimerBook, so that the
expanded dialogue logic is tested independently of Controller I/O.

#### Acceptance Criteria

1. THE test suite SHALL include unit tests for each new Phase 2 per-event
   handler of `DefaultDialogueReducer`, including `HandleTurnTriggerClassified`
2. THE test suite SHALL include unit tests for `TimerBook` covering schedule,
   cancel, next-deadline, pop-expired, empty, and timer-id reuse after cancel
3. THE test suite SHOULD include property-based tests using RapidCheck for the
   expanded reducer and TimerBook
4. THE property-based tests, if included, SHOULD run a minimum of 100
   iterations per property
5. THE test files SHALL be added to the appropriate CMake targets
