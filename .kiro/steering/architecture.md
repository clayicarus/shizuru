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
│  Controller  │         │  IoDevice   │
│  Context     │         │  audio/     │
│  Policy      │         │  asr/       │
│  Strategies  │         │  tts/       │
│              │         │  vad/       │
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
Platform-independent agent logic.  Controller state machine, context
strategy, policy layer, pluggable strategies.  No knowledge of IoDevice,
DataFrame, or any specific vendor.

### io/ — Data Transport Devices
IoDevice implementations that handle physical media or external service
data conversion.  Audio capture/playout, ASR, TTS, VAD, probes.
These devices do NOT make semantic decisions — they only transport and
convert data formats.

### runtime/ — Device Bus + Bridging
- **AgentRuntime**: Pure device bus.  Registers devices, manages routes,
  dispatches frames, controls lifecycle.  Zero business logic.
- **CoreDevice**: Bridge between core/ and io/.  Translates DataFrame ↔
  Observation/ActionCandidate.  The only component that knows both worlds.
- **ToolDispatchDevice + ToolRegistry**: DMA controller analogy.  Receives
  tool call frames from CoreDevice, dispatches to registered tool functions,
  returns results.  Lives in runtime/ because it bridges the agent's
  reasoning loop with external capabilities.
- **RouteTable**: Single source of truth for all data flow topology.

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

## OS-Inspired Design Analogy

The architecture draws from operating system concepts:

| OS Concept | Shizuru Component | Role |
|---|---|---|
| CPU | Controller (core/) | Reasoning, state machine, decision making |
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

1. **Control plane signals** (cancel, flush) originate from CoreDevice
   and flow to IO devices via RouteTable.
2. **Data plane signals** (audio frames, text) flow between IO devices
   via DMA routes (requires_control_plane = false).
3. **Tool call requests** flow from CoreDevice:action_out to
   ToolDispatchDevice:action_in via control plane route.
4. **Tool results** flow from ToolDispatchDevice:result_out back to
   CoreDevice:tool_result_in via control plane route.
5. **Tool side effects** (scheduler writes, DB writes) are executed
   directly by tool functions — they do NOT flow through the bus.
   The Controller authorizes these indirectly via the tool call mechanism.
