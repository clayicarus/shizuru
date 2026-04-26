# Implementation Plan: Dialogue Kernel Phase 3

## Overview

Phase 3 introduces a provisional turn workspace inside `DialogueState` and
event-izes the `ObservationAggregator`.  After Phase 3, debounce fragments
are merged before commit, the aggregator's buffer/flush/timeout behavior is
modeled as typed events, and the reducer controls the commit boundary between
provisional workspace and committed history.

**Hard scope:** Add workspace types, new events/effects, update reducer
handlers for workspace integration, add effect handlers in Controller, rewire
RunLoop aggregator to event-driven, add tests.

**Not in Phase 3:** Agenda, mixed-initiative, clarification (Phase 4).
ContextStrategy API changes.  Playback lifecycle events.

## Tasks

- [x] 1. Add workspace types to `core/dialogue/types.h`
  - [x] 1.1 Define `WorkspaceEntry` struct
    - Fields: `content` (string), `source` (string), `entry_type` (MemoryEntryType), `timestamp` (time_point)
    - _Requirements: 1.5_
  - [x] 1.2 Define `TurnWorkspace` struct
    - Fields: `user_fragments` (vector of WorkspaceEntry), `assistant_partial` (optional WorkspaceEntry)
    - _Requirements: 1.2, 1.3, 1.4_
  - [x] 1.3 Add `workspace` field to `DialogueState`
    - Type: `TurnWorkspace`, default-initialized (empty)
    - _Requirements: 1.1_
  - [x] 1.4 Add Phase 3 event types
    - `UserFragmentReceived{observation, now}` — raw fragment before aggregation
    - `AggregationComplete{observation, now}` — aggregated complete observation
    - `AggregationTimeout{observation, now}` — timeout-flushed content
    - Update `DialogueEvent` variant to include all three
    - _Requirements: 9.1, 9.2, 9.3_
  - [x] 1.5 Add Phase 3 effect types
    - `BufferToWorkspace{entry, is_fragment}` — write to workspace
    - `CommitWorkspace{merge_fragments}` — promote workspace to committed history
    - `DiscardWorkspace{}` — drop workspace without committing
    - Update `DialogueEffect` variant to include all three
    - _Requirements: 2.1, 3.1, 4.1_
  - [x] 1.6 Extend `TimerKind` enum
    - Add `kAggregationTimeout` value
    - _Requirements: 10.4_

- [x] 2. Checkpoint — Verify expanded types compile

- [x] 3. Update reducer handlers for workspace integration
  - [x] 3.1 Update `HandleUserMessage` — debounce path
  - [x] 3.2 Update `HandleUserMessage` — normal path (kIdle)
  - [x] 3.3 Update `HandleUserMessage` — superseding path (kAwaitingTurnTrigger)
  - [x] 3.4 Update `HandleDebounceCooldownExpired`
  - [x] 3.5 Update `HandleTimerExpired` for kDebounce
  - [x] 3.6 Update `HandleInterrupt`
  - [x] 3.7 Add `HandleAggregationComplete` handler
  - [x] 3.8 Add `HandleAggregationTimeout` handler
  - [x] 3.9 Add `HandleUserFragmentReceived` handler
  - [x] 3.10 Update `Reduce` dispatch — wire Phase 3 handlers via `std::visit`

- [x] 4. Update `default_reducer.h` — add Phase 3 handler declarations

- [x] 5. Checkpoint — Verify updated reducer compiles and existing tests pass

- [x] 6. Add workspace effect handlers to Controller
  - [x] 6.1 Implement `BufferToWorkspace` effect handler in `ApplyDialogueDecision`
  - [x] 6.2 Implement `CommitWorkspace` effect handler in `ApplyDialogueDecision`
  - [x] 6.3 Implement `DiscardWorkspace` effect handler in `ApplyDialogueDecision`

- [x] 7. Checkpoint — Verify effect handlers compile and existing tests pass

- [x] 8. Rewire RunLoop aggregator to event-driven
  - [x] 8.1 Update normal user message path in RunLoop
  - [x] 8.2 Update aggregator timeout path in RunLoop
  - [x] 8.3 Add aggregation timeout configuration
  - [x] 8.4 Handle `kAggregationTimeout` in `HandleTimerExpired` reducer

- [x] 9. Checkpoint — Verify aggregator event-ization works

- [x] 10. Write unit tests for Phase 3 reducer handlers
  - [x] 10.1 Create `tests/agent/dialogue_reducer_phase3_test.cpp`

- [ ] 11. Write property tests for Phase 3
  - [ ] 11.1 Update generators in `dialogue_reducer_prop_test.cpp` for Phase 3
  - [ ] 11.2 Property: Reducer purity (extended)
  - [ ] 11.3 Property: Debounce fragments accumulate
  - [ ] 11.4 Property: Debounce expiry commits merged
  - [ ] 11.5 Property: Normal message commits immediately
  - [ ] 11.6 Property: Interrupt with user fragments commits them

- [x] 12. Update test build system
  - [x] 12.1 Add `dialogue_reducer_phase3_test.cpp` to `dialogue_reducer_test` target in `tests/agent/CMakeLists.txt`

- [x] 13. Checkpoint — Verify all Phase 3 tests pass

- [ ] 14. Write controller-level regression test for debounce fragment merging
  - [ ] 14.1 Add regression test to `tests/agent/controller_phase3_test.cpp`
  - [ ] 14.2 Add `controller_phase3_test.cpp` to `controller_test` target in `tests/agent/CMakeLists.txt`

- [x] 15. Checkpoint — Verify all tests pass including controller regression

- [x] 16. Clean up dead code
  - [x] 16.1 Remove `HandleInterrupt()` legacy method from Controller — confirmed dead, removed
  - [x] 16.2 Remove `HandleRouting()` legacy method from Controller — confirmed dead, removed

- [x] 17. Update architecture documentation
  - [x] 17.1 Update `.kiro/steering/architecture.md` — Core Internal Architecture section

- [x] 18. Final checkpoint — Ensure all tests pass

## Notes

- Phase 3 hard scope: workspace types + new events/effects + reducer handler updates + Controller effect handlers + aggregator event-ization + tests
- `ContextStrategy` API is unchanged — `RecordTurn` and `BuildContext` keep their current signatures
- The workspace is entirely internal to `dialogue/` state and Controller effect execution
- `BufferToWorkspace` effect handler is a no-op for external side effects — the reducer already updates `next_state.workspace`. The effect exists for explicitness and future auditing/logging.
- `CommitWorkspace` effect handler handles the empty workspace case as a no-op (no empty entries written)
- Fragment merging uses space-separated concatenation — this is a simple heuristic suitable for Chinese and English text. More sophisticated merging (e.g., removing duplicate prefixes from incremental ASR) can be added later.
- `UserMessageReceived` is retained for backward compatibility. `AggregationComplete` and `AggregationTimeout` are the new primary ingress events. The reducer handles them identically.
- The aggregator's `Feed()` and `CheckTimeout()` methods are still called by Controller (not by the reducer) — the reducer remains pure. The event-ization means the *results* of aggregation are now typed events, not that the aggregator itself runs inside the reducer.
- Phase 2 tests that check for `RecordMemory` in the debounce path will need updating to check for `BufferToWorkspace` instead. This is expected and documented in task 5.
- `kAggregationTimeout` timer handling is split: the reducer receives `TimerExpired{kAggregationTimeout}` as a no-op (it can't call the aggregator), and the Controller's timer handler calls `CheckTimeout()` and feeds the result back as `AggregationTimeout`. This preserves reducer purity.
