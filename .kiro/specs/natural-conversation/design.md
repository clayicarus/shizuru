# Natural Conversation Model

## Architectural Positioning

This document describes the short-term behavior target for natural
conversation.  It should remain valid as a user-visible requirement even if
the implementation moves away from ad-hoc controller branches.

Long term, this behavior should live inside the dialogue kernel described in
`.kiro/steering/dialogue-kernel.md`, where barge-in, debounce, continuation,
and internal timer handling are modeled as dialogue events and effects rather
than special-case controller flags.

## Problem

Current behavior: user sends message A → LLM starts thinking → user sends B →
LLM is interrupted → only B is processed → LLM responds to B alone, ignoring
that A was part of the same thought.

Expected behavior: like a real conversation — if someone is still talking while
you're thinking, you wait until they finish, then respond to everything together.

## Current Flow (Broken)

```
User sends A → Controller enters kThinking → LLM starts
User sends B → barge-in detected:
  1. HandleInterrupt() — cancel LLM
  2. EnqueueObservation(B) — only B goes back to queue
  3. Controller returns to kListening
  4. B is dequeued → aggregator → filter → HandleThinking(B)
  5. If C arrives during B's thinking → another interrupt cycle
```

Result: LLM sees A (in history, with no response) + B (current), but the
response to A was discarded. If user sends A, B, C rapidly, each one
interrupts the previous, and only the last one gets a proper response.

## Target Flow

```
User sends A → Controller enters kThinking → LLM starts
User sends B → barge-in detected:
  1. HandleInterrupt() — cancel LLM
  2. Record A's observation to context (already done by HandleThinking)
  3. Record B to context immediately (RecordTurn)
  4. Do NOT enter HandleThinking yet — stay in kListening
  5. Start a debounce window (reuse aggregator timeout)
User sends C within the window:
  6. Record C to context
  7. Reset debounce timer
Window expires (no new input for N ms):
  8. Enter HandleThinking with a kContinuation observation
  9. BuildContext sees A + B + C in history, no current observation
  10. LLM responds to all three together
```

## Key Insight

The aggregator already implements the debounce window — it buffers inputs
and flushes after a timeout.  The problem is that barge-in bypasses the
aggregator by directly calling HandleInterrupt + EnqueueObservation.

The fix is: **on barge-in, record the new message to context and let the
normal aggregator/filter/timeout flow handle the rest**, instead of
immediately re-entering HandleThinking.

## Implementation

### Change 1: Barge-in records instead of re-enqueuing

In `Controller::RunLoop`, the barge-in branch currently does:

```cpp
if (obs.type == kUserMessage && state in {kThinking, kRouting, kActing}) {
    HandleInterrupt();
    EnqueueObservation(std::move(obs));  // ← problem: skips aggregator
    continue;
}
```

Change to:

```cpp
if (obs.type == kUserMessage && state in {kThinking, kRouting, kActing}) {
    HandleInterrupt();
    // Record the interrupting message to context so LLM sees it next time.
    MemoryEntry entry;
    entry.type = kUserMessage;
    entry.role = "user";
    entry.content = obs.content;
    entry.source_tag = obs.source;
    entry.timestamp = obs.timestamp;
    context_.RecordTurn(session_id_, entry);
    // Do NOT call HandleThinking — stay in kListening.
    // The next RunLoop iteration will either:
    //   a) Pick up another message (user is still typing) → record it too
    //   b) Timeout (user stopped) → aggregator.CheckTimeout() or direct thinking
    continue;
}
```

### Change 2: Post-interrupt thinking trigger

After barge-in, the Controller is in kListening with recorded but unprocessed
messages.  The RunLoop's existing timeout mechanism (aggregator.CheckTimeout
or the 500ms wait) will eventually fire and trigger HandleThinking with a
kContinuation observation.  BuildContext will include all recorded messages.

If the aggregator is not configured (PassthroughAggregator), we need a
fallback: after recording the barge-in message, set a flag that causes the
next RunLoop iteration to enter HandleThinking with a short delay.

### Change 3: Debounce for text input

Text input (from the send button) should also benefit from this model.
If the user rapidly sends multiple messages, they should be collected
and processed together.  This is naturally handled by the aggregator
if text input goes through it.

Currently text input bypasses aggregator because it's "complete" — the
aggregator's LLM endpointing says "yes, this is a complete sentence".
But in the barge-in scenario, we want to wait for more input even if
each message is individually complete.

Solution: after a barge-in, set a `post_interrupt_cooldown_` flag that
forces the next N ms of input to be buffered regardless of aggregator
decision.  This flag is cleared when HandleThinking is finally called.

## State Machine Impact

Short-term implementation can still be expressed within the existing
controller states:

- Before: interrupt → kListening → immediately process next observation
- After: interrupt → kListening → buffer observations → timeout → process all

Long-term architecture should not rely on controller state alone for this
behavior.  The richer model belongs in explicit dialogue state owned by the
dialogue kernel.

## Testing

1. **Barge-in collects multiple messages**: Send A, wait for kThinking,
   send B and C rapidly.  Verify LLM receives A+B+C in context.
2. **Single message still works**: Send A, no barge-in.  Verify normal flow.
3. **Interrupt cancels LLM**: Verify LLM.Cancel() is still called on barge-in.
4. **Timeout triggers thinking**: After barge-in + messages, verify
   HandleThinking is called after the debounce window.
