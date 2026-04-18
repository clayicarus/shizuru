# Implementation Plan: Dialogue Kernel Phase 1

## Overview

Introduce `core/dialogue/` as a pure-logic module containing foundational types, a reducer interface, and a `DefaultDialogueReducer` that owns barge-in and debounce cooldown logic through explicit state + events + effects. Wire the reducer into Controller for barge-in/debounce only, replacing `post_interrupt_cooldown_` with typed cooldown state. Normal message flow, tool result/timeout, and LLM completion handling remain in Controller's existing inline logic for Phase 1.

**Hard scope:** types, reducer interface, barge-in + debounce in reducer, `ApplyDialogueDecision` in Controller.

**Not in Phase 1:** normal message flow via reducer, tool result/timeout via reducer, LLM completion accounting via reducer, full behavioral equivalence property test.

## Tasks

- [x] 1. Create dialogue type definitions (`core/dialogue/types.h`)
  - [x] 1.1 Define `DialogueEvent` variant, `DialogueState` struct, `CooldownPhase` enum, `DialogueEffect` variant, and `DialogueDecision` struct
    - Create `core/dialogue/types.h` with Phase 1 type definitions
    - `DialogueEvent` variant: `InterruptRequested{now}`, `DebounceCooldownExpired{now}`, `UserMessageReceived{observation, now}`, `ShutdownRequested{}`
    - Only include event types that Phase 1 actually handles; other event types (LlmCompleted, LlmFailed, ToolResultReceived, ToolCallTimeout, ContinuationRequested, SystemEventReceived) are declared as empty structs for forward compatibility but not handled by the reducer yet
    - `InterruptRequested` and `DebounceCooldownExpired` MUST include a `now` field (`std::chrono::steady_clock::time_point`) — the reducer must never read the system clock
    - `CooldownPhase` enum: `kNone`, `kDebouncing`
    - `DialogueState` struct: `conversation_active`, `cooldown`, `turn_count`, `total_prompt_tokens`, `total_completion_tokens`, `action_count`, `session_start`, `last_activity`
    - `DialogueEffect` variant: `RecordMemory`, `CancelLlm`, `StartLlmContinuation{now}`, `EmitActivityEffect`, `SignalBudgetExhausted`, `NoOp`
    - NOTE: No `SignalInterrupted` effect — `CancelLlm` is the single effect responsible for cancellation, state transition, and activity notification. This avoids duplicate kInterrupted activity events.
    - `DialogueDecision` struct: `next_state`, `effects`
    - Depends only on `core/controller/types.h`, `core/controller/config.h`, and C++17 standard library
    - No includes from `io/`, `runtime/`, `services/`, or `app/`
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 10.1, 10.3, 13.1, 13.2, 13.3, 13.4_

- [x] 2. Create reducer interface (`core/dialogue/reducer.h`)
  - [x] 2.1 Define `DialogueReducer` abstract class with pure virtual `Reduce` method
    - Create `core/dialogue/reducer.h`
    - Declare `Reduce(const DialogueState& state, const DialogueEvent& event) const` returning `DialogueDecision`
    - Mark `Reduce` as `const` to enforce no internal mutation
    - Include virtual destructor
    - _Requirements: 2.1, 2.2, 2.3_

- [x] 3. Implement DefaultDialogueReducer (barge-in + debounce only)
  - [x] 3.1 Create `core/dialogue/default_reducer.h` with class declaration
    - Declare `DefaultDialogueReducer` implementing `DialogueReducer`
    - Constructor accepts `const ControllerConfig&` and stores immutable snapshot
    - Declare private handlers: `HandleInterrupt`, `HandleDebounceCooldownExpired`, `HandleUserMessageDuringDebounce`
    - Declare private `IsBudgetExhausted` helper (uses `now` parameter, not system clock)
    - _Requirements: 3.1, 3.2_

  - [x] 3.2 Implement `default_reducer.cpp` — dispatch and barge-in/debounce handlers
    - Implement `Reduce` using `std::visit`; unhandled event types return state unchanged with empty effects (no-op passthrough)
    - Implement `HandleInterrupt(state, now)`: set `cooldown = kDebouncing`, update `last_activity = now`, emit `CancelLlm` only (no separate SignalInterrupted — CancelLlm is the single interrupt effect)
    - Implement `HandleDebounceCooldownExpired(state, now)`: set `cooldown = kNone`, check `IsBudgetExhausted(state, now)`, emit `StartLlmContinuation{now}` or `SignalBudgetExhausted`
    - Implement `HandleUserMessageDuringDebounce(state, event)`: if `cooldown == kDebouncing`, emit `RecordMemory` only (no `StartLlmContinuation`), keep cooldown; if `cooldown == kNone`, return no-op (Controller handles normal flow in Phase 1)
    - Implement `IsBudgetExhausted(state, now)`: check turn_count, tokens, action_count, elapsed time using `now - state.session_start` (NOT `steady_clock::now()`)
    - All methods `const`, no I/O, no blocking, no system clock reads
    - _Requirements: 3.3, 3.4, 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 6.1, 6.2, 6.3, 13.1, 13.2_

- [x] 4. Update build system
  - [x] 4.1 Add `dialogue/default_reducer.cpp` to `core/CMakeLists.txt`
    - Add the new source file to the `shizuru_core` STATIC library target
    - _Requirements: 10.2, 10.3_

- [x] 5. Checkpoint — Verify dialogue module compiles
  - Ensure all tests pass, ask the user if questions arise.

- [x] 6. Write unit tests for DefaultDialogueReducer
  - [x] 6.1 Create `tests/agent/dialogue_reducer_test.cpp` with unit tests for barge-in/debounce handlers
    - Test `HandleInterrupt`: verify cooldown set to `kDebouncing`, `CancelLlm` in effects (single effect, no SignalInterrupted), `last_activity` updated to provided `now`
    - Test `HandleUserMessage` during debounce: verify `RecordMemory` in effects, NO `StartLlmContinuation` in effects, cooldown stays `kDebouncing`
    - Test `HandleUserMessage` when cooldown is `kNone`: verify no-op (state unchanged, empty effects) — normal flow stays in Controller for Phase 1
    - Test `HandleDebounceCooldownExpired` with budget OK: verify cooldown cleared to `kNone`, `StartLlmContinuation` in effects
    - Test `HandleDebounceCooldownExpired` with budget exhausted: verify `SignalBudgetExhausted` in effects, NO `StartLlmContinuation`
    - Test `IsBudgetExhausted` boundary values for each limit (turns, tokens, actions, time) — all using explicit `now` parameter
    - Test `ShutdownRequested`: verify state unchanged, empty effects
    - Test unhandled event types (LlmCompleted, ToolResultReceived, etc.): verify no-op passthrough
    - Test reducer purity: call `Reduce` twice with same inputs, verify identical outputs
    - _Requirements: 12.1, 12.5, 13.1_

  - [x] 6.2 Write property test: Reducer purity (Property 1)
    - For any valid `DialogueState` and `DialogueEvent`, calling `Reduce(state, event)` twice with identical inputs produces identical outputs
    - Create `tests/agent/dialogue_reducer_prop_test.cpp`
    - Implement `genDialogueState()`, `genDialogueEvent()` generators
    - Run minimum 100 iterations
    - **Validates: Requirements 3.4, 3.5**

  - [x] 6.3 Write property test: Barge-in enters debounce (Property 2)
    - For any `DialogueState` where `cooldown == kNone`, `Reduce(state, InterruptRequested{now})` produces `next_state.cooldown == kDebouncing`, effects contain `CancelLlm` (and no other interrupt-related effects)
    - **Validates: Requirements 4.1**

  - [x] 6.4 Write property test: Debounce buffers without thinking (Property 3)
    - For any `DialogueState` where `cooldown == kDebouncing` and any `UserMessageReceived`, effects contain `RecordMemory` and do NOT contain `StartLlmContinuation`
    - **Validates: Requirements 4.2, 4.3**

  - [x] 6.5 Write property test: Debounce expiry starts thinking (Property 4)
    - For any `DialogueState` where `cooldown == kDebouncing` and budget not exhausted (using explicit `now`), `Reduce(state, DebounceCooldownExpired{now})` produces `next_state.cooldown == kNone` and effects contain `StartLlmContinuation`
    - **Validates: Requirements 4.4, 13.2**

  - [x] 6.6 Write property test: Budget exhaustion prevents thinking (Property 5)
    - For any `DialogueState` where budget is exhausted (using explicit `now`), `Reduce(state, DebounceCooldownExpired{now})` produces effects containing `SignalBudgetExhausted` and NOT containing `StartLlmContinuation`
    - **Validates: Requirements 4.5, 6.1, 6.2, 6.3**

- [x] 7. Update test build system
  - [x] 7.1 Add `dialogue_reducer_test.cpp` and `dialogue_reducer_prop_test.cpp` to `tests/agent/CMakeLists.txt`
    - Create a new `dialogue_reducer_test` executable target
    - Link against `shizuru_core`, `GTest::gtest_main`, `rapidcheck_gtest`
    - Add `add_test` and set `RC_PARAMS=max_success=100` for property tests
    - _Requirements: 12.2, 12.3, 12.4, 12.5_

- [x] 8. Checkpoint — Verify all reducer tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 9. Wire reducer into Controller (barge-in + debounce only)
  - [x] 9.1 Update `core/controller/controller.h` — add reducer and dialogue state members
    - Add `#include "dialogue/reducer.h"` and `#include "dialogue/types.h"`
    - Add `std::unique_ptr<dialogue::DialogueReducer> reducer_` member
    - Add `dialogue::DialogueState dialogue_state_` member
    - Declare `void ApplyDialogueDecision(const dialogue::DialogueDecision& decision)` private method
    - Declare `void SyncBudgetToDialogueState()` private helper
    - _Requirements: 9.1_

  - [x] 9.2 Update `core/controller/controller.cpp` — construct reducer, implement SyncBudgetToDialogueState and ApplyDialogueDecision
    - Construct `DefaultDialogueReducer` with `config_` in Controller constructor
    - Initialize `dialogue_state_` with matching initial values from existing Controller fields
    - Implement `SyncBudgetToDialogueState()`: copy `turn_count_`, `total_prompt_tokens_`, `total_completion_tokens_`, `action_count_`, `session_start_`, `last_activity_` from Controller fields into `dialogue_state_`. This must be called BEFORE every `Reduce` call so the reducer always sees current values regardless of which Controller code path last updated them (HandleThinking, HandleResponding, HandleActing, ResetBudgetWindow all write these counters).
    - Implement `ApplyDialogueDecision`: update `dialogue_state_` to `decision.next_state`, execute each effect in order via `std::visit`
    - CRITICAL: Effect execution must NOT delegate to old Handle* methods. Each effect has its own inline execution:
      - `CancelLlm` → `llm_->Cancel()` + `if (cancel_) cancel_()` + reset TTS/aggregator buffers + record interrupt memory entry + `TryTransition(Event::kInterrupt)` + `EmitActivity(kInterrupted)` — this is the SINGLE place that emits kInterrupted activity
      - `RecordMemory` → `context_.RecordTurn(session_id_, MemoryEntryFromItem(...))`
      - `StartLlmContinuation` → `TryTransition(Event::kUserObservation)` + build continuation Observation + call `HandleThinking(continuation)` (acceptable: continuation entry point, not the interrupt path)
      - `SignalBudgetExhausted` → `ResetAssistantTurnUiState()` + `TryTransition(Event::kStopConditionMet)` + `EmitActivity(kBudgetExhausted)`
      - `EmitActivityEffect` → `EmitActivity(kind, detail)`
      - `NoOp` → nothing
    - _Requirements: 9.2, 9.5, 9.7, 9.8_

  - [x] 9.3 Replace barge-in and debounce logic in `RunLoop` with reducer calls
    - Replace inline `HandleInterrupt()` + `post_interrupt_cooldown_ = true` + `context_.RecordTurn(...)` with: `SyncBudgetToDialogueState()` + `Reduce(dialogue_state_, InterruptRequested{now})` + `ApplyDialogueDecision` + `Reduce(dialogue_state_, UserMessageReceived{obs, now})` + `ApplyDialogueDecision`
    - Replace inline cooldown expiry logic (`if (post_interrupt_cooldown_) { ... }`) with: `SyncBudgetToDialogueState()` + `Reduce(dialogue_state_, DebounceCooldownExpired{now})` + `ApplyDialogueDecision`
    - Replace `post_interrupt_cooldown_` boolean checks with `dialogue_state_.cooldown == CooldownPhase::kDebouncing`
    - Replace post-interrupt cooldown buffering (`if (post_interrupt_cooldown_) { RecordTurn... }`) with: `SyncBudgetToDialogueState()` + `Reduce(dialogue_state_, UserMessageReceived{obs, now})` + `ApplyDialogueDecision`
    - Controller populates `now` field with `std::chrono::steady_clock::now()` when constructing events
    - _Requirements: 9.3, 9.4, 9.6, 9.8, 13.5_

- [x] 10. Checkpoint — Verify existing tests still pass + barge-in regression test
  - Run `controller_test` and `controller_prop_test` — all must pass without modification
  - Run `controller_bug_condition_test` and `controller_preservation_test` — all must pass
  - Write a dedicated controller-level regression test for the reducer-wired barge-in/debounce path:
    - Scenario: send message A → wait for kThinking → send barge-in message B → verify LLM cancelled, interrupt memory recorded, B recorded to context, cooldown active → send message C during cooldown → verify C recorded, cooldown still active → let cooldown expire → verify continuation thinking starts with A+B+C in context
    - This test exercises the full reducer wiring path that existing tests may not specifically cover
  - Ensure all tests pass, ask the user if questions arise.
  - _Requirements: 11.1, 11.2, 11.3, 11.4_

- [x] 11. Final checkpoint — Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional property-based tests (requirements use SHOULD, not SHALL)
- Phase 1 hard scope: types + reducer interface + barge-in/debounce in reducer + ApplyDialogueDecision
- Normal message flow, tool result/timeout, LLM completion accounting stay in Controller for Phase 1
- The reducer NEVER reads the system clock — all `now` values come from event fields populated by Controller
- `SyncBudgetToDialogueState()` must be called before every `Reduce` call to ensure the reducer sees current budget counters from all Controller write points (HandleThinking, HandleResponding, HandleActing, ResetBudgetWindow)
- No `SignalInterrupted` effect — `CancelLlm` is the single effect that handles cancellation + state transition + activity notification, avoiding duplicate kInterrupted events
- Effect execution in ApplyDialogueDecision must NOT delegate to old Handle* methods (except `HandleThinking` for continuation, which is the thinking entry point, not the interrupt path)
- Unhandled event types pass through as no-ops — the reducer only owns barge-in/debounce in Phase 1
