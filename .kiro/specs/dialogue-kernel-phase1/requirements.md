# Requirements Document

## Introduction

Phase 1 of the dialogue kernel introduces a `core/dialogue/` module containing foundational types, a pure reducer interface, and a `DefaultDialogueReducer` that owns barge-in and debounce cooldown logic through explicit state, events, and effects. The hard scope is limited to: type definitions, reducer interface, barge-in + debounce entering the reducer, and Controller having `ApplyDialogueDecision`. Normal message flow, tool result/timeout handling, LLM completion accounting, and full behavioral equivalence are deferred to Phase 2. No external behavior changes. All existing tests continue to pass.

## Glossary

- **Reducer**: A pure function that takes `(DialogueState, DialogueEvent)` and returns a `DialogueDecision` containing the next state and a list of effects. Performs no I/O, no mutation, no blocking.
- **DialogueState**: Explicit struct capturing conversation-active flag, cooldown phase, budget counters, and timestamps. Replaces scattered Controller booleans.
- **DialogueEvent**: A `std::variant` representing the Phase 1 event taxonomy: user messages, system events, interrupts, debounce expiry, LLM completions/failures, tool results, tool timeouts, continuations, and shutdown.
- **DialogueEffect**: A `std::variant` representing side-effect instructions produced by the Reducer. Executed by the Controller's effect executor. Examples: `RecordMemory`, `StartLlm`, `CancelLlm`, `SignalBudgetExhausted`.
- **DialogueDecision**: A struct containing `next_state` (DialogueState) and `effects` (vector of DialogueEffect). The return type of `Reduce`.
- **CooldownPhase**: An enum with values `kNone` (normal operation) and `kDebouncing` (post-interrupt buffering period).
- **Controller**: The existing event loop shell in `core/controller/`. Owns the observation queue, loop thread, effect execution, and callback bridging. In Phase 1, it delegates barge-in and debounce decisions to the Reducer.
- **Effect_Executor**: The part of Controller that iterates over `DialogueDecision.effects` and performs the corresponding I/O or state transitions. Implemented as `Controller::ApplyDialogueDecision`.
- **Budget**: Session-scoped limits on turns, tokens, actions, and elapsed time. Configured via `ControllerConfig`.
- **Barge_In**: When a user message arrives while the Controller is in Thinking, Routing, or Acting state, interrupting the current turn.
- **Debounce**: A cooldown period after barge-in during which incoming user messages are buffered (recorded to memory) but do not trigger new LLM thinking. Ends on timeout expiry.

## Requirements

### Requirement 1: Dialogue Type Definitions

**User Story:** As a core developer, I want explicit type definitions for dialogue events, state, effects, and decisions, so that turn-taking semantics are expressed as data rather than scattered booleans.

#### Acceptance Criteria

1. THE Dialogue_Module SHALL define `DialogueEvent` as a `std::variant` covering: `UserMessageReceived`, `SystemEventReceived`, `InterruptRequested`, `DebounceCooldownExpired`, `LlmCompleted`, `LlmFailed`, `ToolResultReceived`, `ToolCallTimeout`, `ContinuationRequested`, and `ShutdownRequested`
2. THE Dialogue_Module SHALL define `DialogueState` as a struct containing: `conversation_active` (bool), `cooldown` (CooldownPhase enum), `turn_count` (int), `total_prompt_tokens` (int), `total_completion_tokens` (int), `action_count` (int), `session_start` (time_point), and `last_activity` (time_point)
3. THE Dialogue_Module SHALL define `CooldownPhase` as an enum with values `kNone` and `kDebouncing`
4. THE Dialogue_Module SHALL define `DialogueEffect` as a `std::variant` covering: `RecordMemory`, `StartLlm`, `CancelLlm`, `EmitToolCallFrames`, `DeliverResponse`, `ResetBudgetWindow`, `EmitDiagnosticEffect`, `EmitActivityEffect`, `SignalBudgetExhausted`, and `NoOp`
5. THE Dialogue_Module SHALL define `DialogueDecision` as a struct containing `next_state` (DialogueState) and `effects` (vector of DialogueEffect)
6. THE Dialogue_Module SHALL reside in `core/dialogue/` and depend only on `core/controller/types.h`, `core/controller/config.h`, and C++17 standard library headers

### Requirement 2: Reducer Interface

**User Story:** As a core developer, I want a pure virtual reducer interface, so that dialogue logic can be tested with alternative implementations and the contract is explicit.

#### Acceptance Criteria

1. THE DialogueReducer interface SHALL declare a pure virtual method `Reduce(const DialogueState& state, const DialogueEvent& event) const` returning `DialogueDecision`
2. THE DialogueReducer::Reduce method SHALL be marked `const` to enforce that the Reducer does not mutate its own internal state
3. THE DialogueReducer interface SHALL reside in `core/dialogue/reducer.h`

### Requirement 3: DefaultDialogueReducer — Dispatch and Purity

**User Story:** As a core developer, I want a concrete reducer that dispatches events to per-event handlers, so that each dialogue transition is isolated and testable.

#### Acceptance Criteria

1. THE DefaultDialogueReducer SHALL implement the DialogueReducer interface
2. THE DefaultDialogueReducer SHALL accept a `ControllerConfig` at construction and store it as an immutable snapshot
3. WHEN `Reduce` is called, THE DefaultDialogueReducer SHALL dispatch to the appropriate per-event handler using `std::visit` on the DialogueEvent variant
4. THE DefaultDialogueReducer::Reduce method SHALL perform no I/O, no blocking, and no mutation of shared state
5. WHEN `Reduce` is called twice with identical `DialogueState` and `DialogueEvent` inputs, THE DefaultDialogueReducer SHALL return identical `DialogueDecision` outputs

### Requirement 4: Barge-In and Debounce Handling

**User Story:** As a core developer, I want barge-in and debounce logic expressed as explicit reducer transitions, so that the `post_interrupt_cooldown_` boolean is replaced by a typed cooldown phase.

#### Acceptance Criteria

1. WHEN an `InterruptRequested` event is received, THE DefaultDialogueReducer SHALL set `next_state.cooldown` to `kDebouncing` and include `CancelLlm` in the effects list
2. WHILE `cooldown` is `kDebouncing`, WHEN a `UserMessageReceived` event is received, THE DefaultDialogueReducer SHALL include `RecordMemory` in the effects list and keep `next_state.cooldown` as `kDebouncing`
3. WHILE `cooldown` is `kDebouncing`, WHEN a `UserMessageReceived` event is received, THE DefaultDialogueReducer SHALL NOT include `StartLlm` in the effects list
4. WHEN a `DebounceCooldownExpired` event is received and the budget is not exhausted, THE DefaultDialogueReducer SHALL set `next_state.cooldown` to `kNone` and include `StartLlm` in the effects list
5. WHEN a `DebounceCooldownExpired` event is received and the budget is exhausted, THE DefaultDialogueReducer SHALL set `next_state.cooldown` to `kNone` and include `SignalBudgetExhausted` in the effects list instead of `StartLlm`

### Requirement 5: Normal Message Flow (Phase 2 — not in Phase 1 scope)

**User Story:** As a core developer, I want the reducer to handle normal user messages by recording memory and starting LLM thinking, so that the standard message path is expressed through the reducer.

> **Deferred to Phase 2.** Phase 1 only routes barge-in and debounce through the reducer. Normal message flow continues to use the existing Controller inline logic.

#### Acceptance Criteria

1. _(Phase 2)_ WHEN a `UserMessageReceived` event is received and `cooldown` is `kNone`, THE DefaultDialogueReducer SHALL include `RecordMemory` and `StartLlm` in the effects list
2. _(Phase 2)_ WHEN a `UserMessageReceived` event is received and the conversation has been idle longer than `conversation_idle_timeout`, THE DefaultDialogueReducer SHALL reset budget counters in `next_state` and include `ResetBudgetWindow` in the effects list
3. _(Phase 2)_ WHEN a `UserMessageReceived` event is received, THE DefaultDialogueReducer SHALL set `next_state.conversation_active` to `true` and update `next_state.last_activity`

### Requirement 6: Budget Enforcement

**User Story:** As a core developer, I want budget checks expressed as a pure function within the reducer, so that budget exhaustion is testable independently of Controller I/O.

#### Acceptance Criteria

1. THE DefaultDialogueReducer SHALL evaluate budget exhaustion by checking `turn_count` against `max_turns`, cumulative tokens against `token_budget`, `action_count` against `action_count_limit`, and elapsed time against `turn_timeout`
2. WHEN any single budget limit is exceeded, THE DefaultDialogueReducer::IsBudgetExhausted method SHALL return `true`
3. WHEN `IsBudgetExhausted` returns `true` during `DebounceCooldownExpired` handling, THE DefaultDialogueReducer SHALL include `SignalBudgetExhausted` in the effects list and SHALL NOT include `StartLlm`

### Requirement 7: LLM Completion Handling (Phase 2 — not in Phase 1 scope)

**User Story:** As a core developer, I want the reducer to update budget counters when an LLM call completes, so that token and turn accounting is part of the explicit state.

> **Deferred to Phase 2.** Phase 1 keeps LLM completion handling in Controller's existing inline logic. Budget counters remain in Controller fields and are synced to `dialogue_state_` at barge-in boundaries.

#### Acceptance Criteria

1. _(Phase 2)_ WHEN an `LlmCompleted` event is received, THE DefaultDialogueReducer SHALL increment `next_state.turn_count` by 1
2. _(Phase 2)_ WHEN an `LlmCompleted` event is received, THE DefaultDialogueReducer SHALL add `prompt_tokens` to `next_state.total_prompt_tokens` and `completion_tokens` to `next_state.total_completion_tokens`

### Requirement 8: Error and Timeout Handling (Phase 2 — not in Phase 1 scope)

**User Story:** As a core developer, I want the reducer to handle LLM failures and tool call timeouts as explicit events, so that error recovery paths are testable.

> **Deferred to Phase 2.** Phase 1 keeps error/timeout handling in Controller's existing inline logic.

#### Acceptance Criteria

1. _(Phase 2)_ WHEN an `LlmFailed` event is received, THE DefaultDialogueReducer SHALL return a decision with no active thinking effects and SHALL include `EmitDiagnosticEffect` with the failure reason
2. _(Phase 2)_ WHEN a `ToolCallTimeout` event is received, THE DefaultDialogueReducer SHALL return a decision that records timeout results for missing tool calls
3. WHEN a `ShutdownRequested` event is received, THE DefaultDialogueReducer SHALL return the current state unchanged with an empty effects list
4. WHEN an event does not match the expected state context, THE DefaultDialogueReducer SHALL return the current state unchanged with no effects (no-op)

### Requirement 9: Controller Integration

**User Story:** As a core developer, I want the Controller to delegate barge-in and debounce decisions to the reducer via an `ApplyDialogueDecision` method, so that the reducer is wired into the existing event loop.

#### Acceptance Criteria

1. THE Controller SHALL own a `DialogueReducer` instance (via `std::unique_ptr`) and a `DialogueState` member
2. THE Controller SHALL construct a `DefaultDialogueReducer` with the existing `ControllerConfig`
3. WHEN a barge-in is detected in `RunLoop`, THE Controller SHALL call `Reduce(dialogue_state_, InterruptRequested{})` and apply the returned decision instead of inline boolean manipulation
4. WHEN the post-interrupt cooldown expires in `RunLoop`, THE Controller SHALL call `Reduce(dialogue_state_, DebounceCooldownExpired{})` and apply the returned decision instead of inline cooldown logic
5. THE Controller::ApplyDialogueDecision method SHALL update `dialogue_state_` to `decision.next_state` and execute each effect in `decision.effects` in order
6. THE Controller SHALL replace the `post_interrupt_cooldown_` boolean with `dialogue_state_.cooldown` for all cooldown checks
7. THE Controller::ApplyDialogueDecision SHALL NOT delegate effects back to existing Controller Handle* methods (e.g., `StartLlm` must NOT call `HandleThinking`, `SignalInterrupted` must NOT call `HandleInterrupt`). Each effect must be executed with its own inline logic so the reducer is the true semantic boundary, not a wrapper around existing methods.
8. BEFORE calling `Reduce`, THE Controller SHALL sync its canonical budget counters (`turn_count_`, `total_prompt_tokens_`, `total_completion_tokens_`, `action_count_`, `session_start_`, `last_activity_`) into `dialogue_state_`, so the reducer always reads current values regardless of which Controller code path last updated them.

### Requirement 10: Module Isolation

**User Story:** As a core developer, I want the dialogue module to have zero dependencies on `io/`, `runtime/`, `services/`, or `app/`, so that it remains a pure-logic module testable in isolation.

#### Acceptance Criteria

1. THE Dialogue_Module SHALL NOT include any headers from `io/`, `runtime/`, `services/`, or `app/` directories
2. THE Dialogue_Module SHALL compile as part of the `shizuru_core` CMake target by adding `dialogue/default_reducer.cpp` to `core/CMakeLists.txt`
3. THE Dialogue_Module SHALL NOT introduce any new external dependencies beyond the C++17 standard library and existing `core/` headers

### Requirement 11: Scoped Behavioral Equivalence

**User Story:** As a core developer, I want the reducer integration to produce identical barge-in and debounce behavior to the current Controller, so that the refactor is safe for the paths it touches.

> **Scoped to barge-in + debounce only.** Full behavioral equivalence across all observation sequences is a Phase 2 goal. Phase 1 only guarantees equivalence for the code paths that are actually routed through the reducer.

#### Acceptance Criteria

1. WHEN the Controller processes barge-in and debounce sequences through the reducer path, THE Controller SHALL produce the same LLM cancellations, memory recordings, and cooldown-to-thinking transitions as the current Controller without the reducer
2. THE existing `controller_test.cpp` and `controller_prop_test.cpp` test suites SHALL pass without modification after the reducer is wired in
3. THE Dialogue_Module SHALL NOT alter any behavior visible to external callers of Controller (callbacks, emitted frames, state queries)
4. THE test suite SHALL include a dedicated controller-level regression test that exercises the full barge-in/debounce path through the reducer wiring: barge-in during thinking → LLM cancel → interrupt memory recorded → barge-in message recorded → cooldown active → additional messages buffered → cooldown expires → continuation thinking starts with all messages in context

### Requirement 12: Testing Infrastructure

**User Story:** As a core developer, I want dedicated unit and property-based test files for the reducer, so that dialogue logic is tested independently of Controller I/O.

#### Acceptance Criteria

1. THE test suite SHALL include `tests/agent/dialogue_reducer_test.cpp` with unit tests for each per-event handler of DefaultDialogueReducer
2. THE test suite SHOULD include `tests/agent/dialogue_reducer_prop_test.cpp` with property-based tests using RapidCheck
3. THE property-based tests, if included, SHOULD include generators for `DialogueState`, `DialogueEvent`, and `ControllerConfig`
4. THE property-based tests, if included, SHOULD run a minimum of 100 iterations per property
5. THE test files SHALL be added to `tests/agent/CMakeLists.txt`

### Requirement 13: Explicit Time Input

**User Story:** As a core developer, I want the reducer to receive all time information as explicit input parameters, so that it never reads the system clock directly and remains deterministic and testable.

#### Acceptance Criteria

1. THE DialogueReducer::Reduce method SHALL NOT call `std::chrono::steady_clock::now()` or any other system clock function
2. ALL time-dependent decisions in the reducer (budget elapsed time checks, timestamp updates to `last_activity` and `session_start`) SHALL use a `now` timestamp provided as a field on the relevant `DialogueEvent` structs or as an explicit parameter
3. THE `InterruptRequested` event struct SHALL include a `now` field of type `std::chrono::steady_clock::time_point`
4. THE `DebounceCooldownExpired` event struct SHALL include a `now` field of type `std::chrono::steady_clock::time_point`
5. THE Controller SHALL populate the `now` field with `std::chrono::steady_clock::now()` when constructing events before passing them to the reducer
