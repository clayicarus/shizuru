# Requirements Document: Dialogue Kernel Phase 3

## Introduction

Phase 3 of the dialogue kernel introduces a provisional turn workspace that
separates in-progress turn data from committed conversation history.  It also
event-izes the `ObservationAggregator`, replacing the synchronous pre-reducer
step in `Controller::RunLoop` with explicit dialogue events and effects.

After Phase 3, the reducer controls when user input fragments and assistant
output cross the boundary from provisional workspace into committed history.
This eliminates three concrete problems in the current implementation:

1. **Barge-in fragment pollution**: during debounce, every user message is
   immediately committed via `RecordMemory`.  The LLM sees multiple partial
   fragments ("你", "你好", "你好吗") as separate history entries.
2. **Interrupted output pollution**: when the assistant is interrupted
   mid-stream, a "Turn interrupted" entry is committed but the partial
   assistant output that preceded it is lost or inconsistently recorded.
3. **Aggregator opacity**: the aggregator's buffer/flush/timeout behavior is
   an implicit synchronous step invisible to the reducer, making it
   untestable as a state transition and preventing the reducer from reasoning
   about incomplete input.

**Scope boundary:** Phase 3 does NOT introduce agenda, mixed-initiative
behavior, clarification flows, or playback lifecycle events.  Those belong to
Phase 4.  Phase 3 also does NOT change the `ContextStrategy` public API —
`RecordTurn` and `BuildContext` keep their current signatures.  The workspace
is entirely internal to `dialogue/` and `Controller` effect execution.

**Behavioral continuity:** After Phase 3, the external behavior visible to
the LLM and UI must be equivalent to Phase 2 for all non-barge-in paths.
For barge-in paths, the improvement is that debounce fragments are merged
before commit, producing cleaner context.

## Glossary

- **Turn Workspace**: A provisional buffer inside `DialogueState` that holds
  user input fragments and assistant output belonging to the current
  unfinished turn.  Entries in the workspace are NOT visible to
  `ContextStrategy::BuildContext` until they are promoted via
  `CommitWorkspace`.
- **Committed History**: Memory entries that have been written to
  `ContextStrategy` via `RecordTurn` and are guaranteed to appear in future
  prompt windows.  This is the existing behavior — Phase 3 does not change
  how committed history works.
- **CommitWorkspace**: A new `DialogueEffect` that promotes workspace entries
  to committed history.  The effect executor calls `RecordTurn` for each
  promoted entry.
- **Workspace Entry**: A `MemoryEntry` held in the workspace buffer, annotated
  with a source tag and timestamp.  Multiple workspace entries from the same
  debounce window can be merged into a single committed entry.
- **Fragment Merging**: The process of combining multiple user input fragments
  (accumulated during debounce or aggregation) into a single coherent
  `MemoryEntry` before committing.  Merging concatenates content strings with
  a space separator and uses the latest timestamp.
- **ObservationAggregator Event-ization**: Replacing the synchronous
  `Feed()`/`CheckTimeout()` calls in `RunLoop` with dialogue events
  (`UserFragmentReceived`, `AggregationComplete`, `AggregationTimeout`) so
  the reducer can reason about incomplete input.

## Requirements

### Requirement 1: Turn Workspace in DialogueState

**User Story:** As a core developer, I want the dialogue state to contain a
provisional workspace buffer, so that the reducer can hold user fragments and
assistant output without immediately committing them to history.

#### Acceptance Criteria

1. THE DialogueState SHALL contain a `workspace` field of type
   `TurnWorkspace`
2. THE TurnWorkspace SHALL contain a `user_fragments` field
   (`std::vector<WorkspaceEntry>`) for buffered user input
3. THE TurnWorkspace SHALL contain an `assistant_partial` field
   (`std::optional<WorkspaceEntry>`) for in-progress assistant output
4. THE TurnWorkspace SHALL contain a `committed` field (`bool`, default
   `false`) indicating whether the current workspace has been promoted
5. A `WorkspaceEntry` SHALL contain `content` (string), `source` (string),
   `timestamp` (time_point), and `entry_type` (MemoryEntryType)
6. THE workspace SHALL be cleared when a new user turn begins (after commit
   or discard of the previous workspace)

### Requirement 2: CommitWorkspace Effect

**User Story:** As a core developer, I want an explicit effect that promotes
workspace entries to committed history, so that the commit boundary is under
the reducer's control.

#### Acceptance Criteria

1. THE DialogueEffect variant SHALL include `CommitWorkspace{merge_fragments}`
   where `merge_fragments` is a bool indicating whether user fragments should
   be merged into a single entry
2. WHEN `CommitWorkspace` is executed with `merge_fragments = true`, THE
   effect executor SHALL merge all `user_fragments` into a single
   `MemoryEntry` (concatenated content, latest timestamp, combined source)
   and call `RecordTurn` once
3. WHEN `CommitWorkspace` is executed with `merge_fragments = false`, THE
   effect executor SHALL call `RecordTurn` for each workspace entry
   individually
4. WHEN `CommitWorkspace` is executed and `assistant_partial` is present, THE
   effect executor SHALL commit it as a separate `MemoryEntry`
5. AFTER `CommitWorkspace` execution, THE effect executor SHALL clear the
   workspace in `dialogue_state_`

### Requirement 3: DiscardWorkspace Effect

**User Story:** As a core developer, I want an explicit effect that discards
workspace entries without committing, so that interrupted or invalid turn data
can be cleanly dropped.

#### Acceptance Criteria

1. THE DialogueEffect variant SHALL include `DiscardWorkspace{}`
2. WHEN `DiscardWorkspace` is executed, THE effect executor SHALL clear the
   workspace in `dialogue_state_` without calling `RecordTurn`
3. THE reducer SHALL produce `DiscardWorkspace` when an interrupt arrives and
   the workspace contains uncommitted assistant output

### Requirement 4: BufferToWorkspace Effect

**User Story:** As a core developer, I want an explicit effect that writes
entries to the workspace instead of directly to committed history, so that
the reducer can accumulate fragments before deciding to commit.

#### Acceptance Criteria

1. THE DialogueEffect variant SHALL include
   `BufferToWorkspace{entry, is_fragment}` where `entry` is a
   `WorkspaceEntry` and `is_fragment` indicates whether this is a partial
   input that should be merged on commit
2. WHEN `BufferToWorkspace` is executed, THE effect executor SHALL append the
   entry to the appropriate workspace collection (`user_fragments` if
   `is_fragment` or user type, `assistant_partial` if assistant type)
3. THE effect executor SHALL NOT call `RecordTurn` when executing
   `BufferToWorkspace`

### Requirement 5: Debounce Fragment Merging

**User Story:** As a core developer, I want user messages received during
debounce to be buffered in the workspace and merged on commit, so that the
LLM sees a single coherent message instead of multiple fragments.

#### Acceptance Criteria

1. WHEN a `UserMessageReceived` event arrives during debounce
   (`cooldown == kDebouncing`), THE reducer SHALL produce
   `BufferToWorkspace` instead of `RecordMemory`
2. WHEN debounce expires (`DebounceCooldownExpired` or
   `TimerExpired{kDebounce}`), THE reducer SHALL produce
   `CommitWorkspace{merge_fragments=true}` before `StartLlmContinuation`
3. WHEN the workspace contains multiple user fragments at commit time, THE
   effect executor SHALL merge them into a single `MemoryEntry` with
   concatenated content (space-separated), the latest timestamp, and the
   source from the last fragment
4. WHEN the workspace contains zero user fragments at commit time (debounce
   expired with no new messages), THE effect executor SHALL skip the commit
   (no empty entries written)

### Requirement 6: Normal Message Path — Workspace Integration

**User Story:** As a core developer, I want normal (non-debounce) user
messages to go through the workspace for consistency, while maintaining
immediate commit semantics for the non-barge-in path.

#### Acceptance Criteria

1. WHEN a `UserMessageReceived` event arrives with `cooldown == kNone` and
   `deliberation == kIdle`, THE reducer SHALL produce
   `BufferToWorkspace` followed by `CommitWorkspace{merge_fragments=false}`
   followed by `StartTurnTriggerClassification`
2. THE net effect SHALL be equivalent to the current `RecordMemory` behavior:
   the message is committed immediately, but it passes through the workspace
   for uniformity
3. THE existing turn-trigger classification flow SHALL be unchanged

### Requirement 7: Interrupt Workspace Handling

**User Story:** As a core developer, I want interrupts to discard uncommitted
assistant output from the workspace, so that partial responses do not pollute
committed history.

#### Acceptance Criteria

1. WHEN an `InterruptRequested` event arrives and the workspace contains
   `assistant_partial`, THE reducer SHALL produce `DiscardWorkspace` before
   other interrupt effects
2. WHEN an `InterruptRequested` event arrives and the workspace contains only
   `user_fragments` (no assistant output), THE reducer SHALL produce
   `CommitWorkspace{merge_fragments=true}` to preserve user input before
   entering debounce
3. THE `RecordInterruptMemory` effect SHALL continue to record "Turn
   interrupted" as before

### Requirement 8: Assistant Output Workspace Tracking

**User Story:** As a core developer, I want the reducer to track assistant
output in the workspace, so that interrupted streaming output can be
discarded or committed intentionally.

#### Acceptance Criteria

1. WHEN a `DeliverResponse` effect is produced, THE reducer SHALL first
   produce `BufferToWorkspace` with the assistant response, then
   `CommitWorkspace{merge_fragments=false}`
2. THE `HandleResponding` method in Controller SHALL continue to handle
   streaming UI updates and TTS — the workspace only tracks the final
   committed content
3. WHEN an interrupt arrives during streaming (before `DeliverResponse`), THE
   workspace `assistant_partial` SHALL be empty (streaming is effect-executor
   level, not reducer level), so no discard is needed for streaming-only
   interrupts

### Requirement 9: ObservationAggregator Event-ization

**User Story:** As a core developer, I want the aggregator's buffer/flush
behavior modeled as dialogue events, so that the reducer can reason about
incomplete input and aggregation timeout.

#### Acceptance Criteria

1. THE DialogueEvent variant SHALL include `UserFragmentReceived{observation,
   now}` for raw ASR/input fragments before aggregation
2. THE DialogueEvent variant SHALL include `AggregationComplete{observation,
   now}` for the aggregated complete observation
3. THE DialogueEvent variant SHALL include `AggregationTimeout{observation,
   now}` for timeout-flushed content
4. WHEN a raw user observation arrives in `RunLoop`, THE Controller SHALL
   call `aggregator_->Feed(obs)` and map the result to either
   `AggregationComplete` (if Feed returns a value) or buffer silently (if
   Feed returns nullopt, the fragment is held by the aggregator)
5. WHEN `aggregator_->CheckTimeout()` returns a value, THE Controller SHALL
   construct `AggregationTimeout` and feed it to the reducer
6. THE reducer SHALL treat `AggregationComplete` and `AggregationTimeout`
   identically to the current `UserMessageReceived` handling (buffer or
   commit + classify)
7. THE `UserMessageReceived` event type SHALL be retained for backward
   compatibility but the primary ingress path for user input SHALL be
   `AggregationComplete` / `AggregationTimeout`

### Requirement 10: Aggregation Timer Management

**User Story:** As a core developer, I want aggregation timeouts managed by
TimerBook instead of implicit `wait_for` polling, so that timeout behavior
is explicit and testable.

#### Acceptance Criteria

1. WHEN the aggregator buffers a fragment (Feed returns nullopt), THE
   Controller SHALL schedule a timer via `ScheduleTimer{kAggregationTimeout,
   "aggregation", deadline}` where deadline is based on the aggregator's
   configured timeout
2. WHEN the aggregator returns a complete observation (Feed returns value),
   THE Controller SHALL cancel the aggregation timer via
   `CancelTimer{"aggregation"}`
3. WHEN `TimerExpired{kAggregationTimeout}` fires, THE Controller SHALL call
   `aggregator_->CheckTimeout()` and feed the result to the reducer
4. THE `TimerKind` enum SHALL be extended with `kAggregationTimeout`

### Requirement 11: Reducer Purity Preservation

**User Story:** As a core developer, I want the reducer to remain pure after
Phase 3 changes, with no I/O, no blocking, and deterministic outputs.

#### Acceptance Criteria

1. THE reducer SHALL NOT call `aggregator_->Feed()` or any aggregator method
   — aggregation remains in the Controller effect executor layer
2. THE reducer SHALL NOT read the system clock — all timestamps come from
   event fields
3. WHEN `Reduce` is called twice with identical inputs, it SHALL produce
   identical outputs
4. THE workspace manipulation in the reducer SHALL be pure state updates
   (appending to vectors, setting optionals) with no side effects

### Requirement 12: Behavioral Equivalence

**User Story:** As a core developer, I want Phase 3 to preserve external
behavior for non-barge-in paths and improve behavior for barge-in paths.

#### Acceptance Criteria

1. THE existing `controller_test.cpp`, `controller_prop_test.cpp`,
   `controller_phase2_test.cpp`, `controller_bug_condition_test.cpp`, and
   `controller_preservation_test.cpp` SHALL pass without modification
2. THE existing `dialogue_reducer_test.cpp`,
   `dialogue_reducer_phase2_test.cpp`, and
   `dialogue_reducer_prop_test.cpp` SHALL pass without modification (Phase 1
   and Phase 2 tests remain valid)
3. FOR non-barge-in user message sequences, THE LLM context window SHALL
   contain the same entries as before Phase 3
4. FOR barge-in sequences with multiple debounce fragments, THE LLM context
   window SHALL contain a single merged user entry instead of multiple
   fragment entries

### Requirement 13: Module Isolation

**User Story:** As a core developer, I want the dialogue module to maintain
zero dependencies on `io/`, `runtime/`, `services/`, or `app/`.

#### Acceptance Criteria

1. THE dialogue module SHALL NOT include headers from `io/`, `runtime/`,
   `services/`, or `app/`
2. THE new workspace types SHALL depend only on existing `core/` types and
   C++17 standard library headers

### Requirement 14: Testing

**User Story:** As a core developer, I want dedicated tests for workspace
behavior, fragment merging, and aggregator event-ization.

#### Acceptance Criteria

1. THE test suite SHALL include unit tests for workspace buffer/commit/discard
   operations in the reducer
2. THE test suite SHALL include unit tests for fragment merging logic
3. THE test suite SHALL include unit tests for the aggregator event-ization
   path (AggregationComplete, AggregationTimeout)
4. THE test suite SHOULD include property tests for workspace invariants
5. THE test suite SHALL include a controller-level regression test for
   debounce fragment merging: send message A → barge-in → send fragments
   B, C during debounce → debounce expires → verify context contains one
   merged entry "B C" (not two separate entries)
