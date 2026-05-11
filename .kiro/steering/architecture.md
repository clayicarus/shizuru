---
inclusion: auto
---

# Shizuru Architecture Guide

This document defines the layering, module responsibilities, and design
decisions for the Shizuru codebase.  All contributors (human and AI) should
follow these conventions.

## Layer Diagram

```
┌─────────────────────────────────────────────┐
│              UI Layer (Flutter)              │
│  Dart: conversation UI, debug panel         │
└──────────────┬──────────────────────────────┘
               │ dart:ffi
┌──────────────▼──────────────────────────────┐
│         C Bridge (ui/bridge/)               │
│  Thin C API: config parsing, platform audio │
│  device creation, C callback wiring         │
└──────────────┬──────────────────────────────┘
               │
┌──────────────▼──────────────────────────────┐
│         App Layer (app/)                    │
│  Product logic: AppRuntime, persona,        │
│  scheduler, tools, memory persistence       │
└──────────────┬──────────────────────────────┘
               │
┌──────────────▼──────────────────────────────┐
│         Runtime Layer (runtime/)            │
│  AgentRuntime (device bus), CoreDevice,     │
│  ToolDispatchDevice, ToolRegistry, RouteTable│
└──────┬───────────────────────┬──────────────┘
       │                       │
┌──────▼──────┐         ┌──────▼──────┐
│  Core (core/)│         │  IO (io/)   │
│  Dialogue    │         │  IoDevice   │
│  Controller  │         │  audio/     │
│  Context     │         │  asr/       │
│  Policy      │         │  tts/       │
│  Strategies  │         │  vad/       │
│              │         │  probe/     │
└──────────────┘         └──────────────┘
       │
┌──────▼──────────────────────────────────────┐
│         Services (services/)                │
│  Vendor clients: OpenAI, Baidu, ElevenLabs  │
│  Memory (InMemoryStore), Audit (LogAuditSink)│
└─────────────────────────────────────────────┘
```

## Dependency Rules

- `io/` does NOT depend on `core/` or `runtime/`
- `core/` does NOT depend on `io/` or `runtime/`
- `runtime/` depends on both `core/` and `io/` (it bridges them)
- `app/` depends on `runtime/`, `core/`, `services/`
- `ui/bridge/` depends on `app/`, `io/` (for platform audio devices)
- `services/` does NOT depend on `core/`, `io/`, or `runtime/`

## Module Responsibilities

### core/ — Agent Framework
Platform-independent agent logic.  Dialogue kernel, controller shell,
context strategy, policy layer, and pluggable strategies.  No knowledge of
IoDevice, DataFrame, or any specific vendor.

`core/` is internally split into:
- **dialogue/**: Dialogue kernel for turn-taking, reducer/effect semantics,
  agenda management, and internal event handling.
- **controller/**: Event loop shell that owns the queue, executes effects,
  and bridges asynchronous callbacks back into dialogue events.
- **context/**: Conversation history, prompt-window assembly, summarization.
- **policy/**: Capability checks, approvals, audit hooks.
- **strategies/**: Pluggable classifiers and helpers (input aggregation,
  relevance filtering, TTS segmentation, response filtering).

Current implementation status:
- **Phase 1 is implemented**.  `core/dialogue/DefaultDialogueReducer`
  owns the text/message-layer **barge-in + debounce cooldown** branch.
- **Phase 2 is implemented**.  All post-aggregation turn-taking semantics
  (normal user messages, LLM completion/failure, tool result/timeout,
  system events, continuation, turn-trigger classification) are routed
  through the reducer.  Controller is a thin event-mapping and
  effect-execution shell.
- **Turn-trigger classification is temporarily bypassed at runtime**.
  The reducer/effect shape exists, but Controller currently maps
  `StartTurnTriggerClassification` directly to `kRespondNow` for all
  meaningful observations.  This is intentional until a robust,
  cancellable classifier path with explicit shutdown semantics is
  reintroduced.
- **Phase 3 is implemented**.  A provisional turn workspace separates
  in-progress turn data from committed history.  Debounce fragments are
  merged before commit.  The ObservationAggregator is event-driven
  (AggregationComplete / AggregationTimeout events, TimerBook-managed
  timeout).
- **Phase 4 (agenda, mixed-initiative)** is not yet implemented.

### io/ — Data Transport Devices
IoDevice implementations that handle physical media or external service
data conversion.  Audio capture/playout, ASR, TTS, VAD, probes.
These devices do NOT make semantic decisions — they only transport and
convert data formats.

### runtime/ — Device Bus + Bridging
- **AgentRuntime**: Pure device bus.  Registers devices, manages routes,
  dispatches frames, controls lifecycle.  Zero business logic.
- **CoreDevice**: Bridge between core/ and io/.  Translates DataFrame ↔
  observation/tool-result/interrupt semantics.  The only component that knows
  both the bus contract and the core dialogue contract.
- **ToolDispatchDevice + ToolRegistry**: DMA controller analogy.  Receives
  tool call frames from CoreDevice, dispatches to registered tool functions,
  returns results.  Lives in runtime/ because it bridges the agent's
  reasoning loop with external capabilities.
- **RouteTable**: Single source of truth for signal topology across the bus.

### app/ — Product Layer
Product-specific logic built on top of the infrastructure layers.
- **AppRuntime**: Assembles CoreDevice + ToolDispatchDevice + Scheduler,
  wires routes, registers tools, manages persona prompt.
- **Persona**: System prompt assembly (fixed personality + dynamic context).
- **Scheduler**: Timer-based IoDevice for reminders and followups.
- **Tools**: Builtin tool function registration.
- **Memory**: Persistent storage (SQLite), structured memory types.

### services/ — Vendor Clients
HTTP clients for external APIs.  OpenAI LLM, Baidu ASR/TTS, ElevenLabs
TTS, token managers.  Pure client implementations with no framework
knowledge.

### ui/bridge/ — C API Bridge
Thin C API layer for Flutter dart:ffi.  Holds AppRuntime, creates
platform-specific audio devices (Oboe vs PortAudio), wires C callbacks
for Dart NativeCallable.  No business logic.

## Core Internal Architecture

The `core/` architecture implements a unified semantic pipeline:

### Data Model

```
Perception Layer          Interpretation Layer       Semantic Layer
─────────────────         ────────────────────       ──────────────
AudioFrame (raw)    →     ASR Interpreter      →     ConversationItem
OneBot JSON (raw)   →     OneBot Parser        →     ConversationItem
UI text (raw)       →     Flutter Adapter      →     ConversationItem
Timer event (raw)   →     Scheduler Adapter    →     ConversationItem
```

### Core Types

- **`ConversationItem`** (`core/conversation_item.h`): Source-final semantic unit.
  Contains: item_id, conversation_id, kind, actor, parts (ContentParts),
  wall_time, reply_to_item_id, mentions.
- **`ContentPart`** (`core/content_part.h`): Variant of TextPart, ImagePart,
  AudioPart, ToolCallPart, ToolResultPart.
- **`InvokeBatch`** (`core/invoke_batch.h`): One LLM call's input unit.
  Groups multiple ConversationItems with a TriggerReason.
- **`ControlSignal`** (`core/control_signal.h`): Control plane variant
  (Flush, Cancel, Interrupt, ThinkStart, ToolCallStart, FinalAnswerStart,
  TurnComplete, ToolResult).

### Input Path

1. Interpreters deliver `ConversationItem` to `CoreDevice::OnConversationItem()`
2. Core's `Batcher` accumulates items in a pending queue
3. Turn policy decides when to flush → constructs `InvokeBatch`
4. `provider_render` projects InvokeBatch + history → OpenAI messages JSON
5. `LlmClient::SubmitStreaming()` sends to LLM

### Output Path

1. LLM raw output → `OutputInterpreter`
2. OutputInterpreter produces:
   - `ConversationItem(kAssistantMessage)` → history
   - `ConversationItem(kToolCall)` → history + tool dispatch
   - `ControlSignal` (ThinkStart, TurnComplete, etc.)

### Tool Result Path

1. `ToolDispatchDevice` executes tool → emits `ToolResultSignal`
2. `CoreDevice::OnControl()` receives signal
3. Core constructs `ConversationItem(kToolResult)` → writes to history
4. Triggers tool continuation (re-invokes LLM)

### History

- `HistoryStore` interface (`core/history.h`): Append, GetWindow, GetRecent, Clear
- `InMemoryHistory` (`services/memory/in_memory_history.h`): Development/testing
- `SqliteMemoryStore` (`app/memory/`): Persistent storage, JSON serialization

### Dialogue Kernel

The dialogue kernel (`core/dialogue/`) owns turn-taking semantics:
- **DialogueEvent**: ConversationItemReceived, ToolResultReceived,
  SystemEventReceived, InterruptRequested, LlmCompleted, etc.
- **DialogueEffect**: RecordConversationItem, StartLlmWithBatch,
  RecordToolResultItem, CancelLlm, ScheduleTimer, etc.
- **DialogueReducer**: Pure `(state, event) → (next_state, effects)`

The Controller remains the event loop shell and effect executor.

## OS-Inspired Design Analogy

The architecture draws from operating system concepts:

| OS Concept | Shizuru Component | Role |
|---|---|---|
| CPU | Controller shell + dialogue reducer (core/) | Reasoning, state transitions, decision making |
| Memory Manager | ContextStrategy (core/) | Prompt window, token budget |
| Permission System | PolicyLayer (core/) | RBAC, audit, approval gates |
| Device Bus | AgentRuntime (runtime/) | Device registration, frame routing |
| Device Driver | IoDevice implementations (io/) | Data transport, format conversion |
| DMA Controller | ToolDispatchDevice (runtime/) | Async tool execution, result delivery |
| Interrupt Vector Table | ToolRegistry (runtime/) | Tool name → function mapping |
| Interrupt | Tool result frame | Signals completion, resumes CPU |

### Where the analogy holds
- Audio pipeline: capture → VAD → ASR → playout behaves exactly like a
  DMA data path — high-frequency data flows without CPU involvement.
- Control signals (cancel, flush) from CoreDevice to IO devices behave
  like hardware interrupts.
- Tool calls: CPU issues IO request → DMA controller dispatches → device
  executes → interrupt signals completion → CPU resumes.

### Where the analogy is relaxed
- Tool functions may have side effects (writing to scheduler, database).
  This is like a DMA handler writing to another device's registers —
  it's the DMA layer's implementation detail, not the CPU's concern.
- The CPU (Controller) authorizes tool execution via the tool call
  mechanism; the DMA controller (ToolDispatchDevice) handles dispatch
  and side effects.

## Signal Flow Rules

1. **Control plane signals** (cancel, flush) flow to IO devices via
   RouteTable.  They are usually emitted by CoreDevice, but signal adapters
   may also emit device-local control commands (for example speech_end →
   ASR flush).
2. **Data plane signals** (audio frames, text) flow between IO devices
   via DMA routes (requires_control_plane = false).
3. **Event plane signals** (interrupts, scheduler triggers) enter core via
   dedicated semantic input ports rather than device-specific protocols.
4. **Tool call requests** flow from CoreDevice:action_out to
   ToolDispatchDevice:action_in via control plane route.
5. **Tool results** flow from ToolDispatchDevice:result_out back to
   CoreDevice:tool_result_in via control plane route.
6. **Tool side effects** (scheduler writes, DB writes) are executed
   directly by tool functions — they do NOT flow through the bus.
   The Controller authorizes these indirectly via the tool call mechanism.

## Context Window Source Identification

Non-user inputs in the LLM context window are distinguished using the
OpenAI API's native `name` field on `role: "user"` messages.  This is
preferred over in-content markup (XML tags, etc.) because:
- It's a structured API field, not text the LLM has to parse
- LLMs natively understand `name` as a speaker identifier
- Zero overhead for the most common case (plain user text)

### How it works

1. `Observation.source` is set by CoreDevice based on the input port
2. `Controller::HandleThinking` copies `obs.source` → `MemoryEntry.source_tag`
3. `ContextStrategy::BuildContext` maps `source_tag` → `ContextMessage.name`
4. `MessageToJson` serializes `name` into the API request

### Defined source names

| name | Origin | Notes |
|------|--------|-------|
| (empty) | User text input | Default — no name field sent |
| `user` | User text input | Explicit user attribution |
| `voice` | ASR transcript | May have recognition errors |
| `scheduler` | Reminder / followup trigger | Payload is JSON data, not user speech |
| `tool` | Tool result | Already uses `role: "tool"` |

### Design Principles

1. **Event payloads are structured data (JSON)** — the LLM decides how
   to express them based on persona prompt guidance.  Never hard-code
   natural language instructions in payloads.
2. **Persona prompt explains name semantics** — tells the LLM how to
   handle messages from different sources (e.g., scheduler events should
   be brought up naturally, not announced as system notifications).
3. **The `name` field pipeline is already fully wired** — Observation.source
   → MemoryEntry.source_tag → ContextMessage.name → API `name` field.
