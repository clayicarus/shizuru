# Shizuru — Agent Guidelines

All markdown documents must be written in English except spec.

## Development Philosophy

This project is in its early stages. Always solve problems directly — no workarounds, no shortcuts, no deferred hacks. If something is broken, fix the root cause.

## Architecture

See `.kiro/steering/architecture.md` for the authoritative architecture guide, including:
- Layer diagram and dependency rules
- Module responsibilities (core/, io/, runtime/, app/, services/, ui/)
- OS-inspired design analogy (CPU, DMA controller, interrupt model)
- Signal flow rules (control plane, data plane, tool calls)

## Technology Stack

- Core runtime (agent framework + audio): C++17
- App layer (product logic): C++17
- UI layer: Flutter (Dart), communicates with C++ via dart:ffi
- LLM integration: OpenAI compatible API (HTTP + SSE streaming via libcurl)
- Build system: CMake (C++ core + app), Flutter toolchain (UI)
- All AI model calls use API-based invocation, no local model inference

## Cross-Platform Strategy

The C++ core is the single shared codebase across all platforms.
Platform-specific code is isolated behind abstract interfaces (e.g., audio backends).

| Platform      | Audio Backend | UI      | Agent Core |
|---------------|---------------|---------|------------|
| macOS / Linux | PortAudio     | Flutter | C++        |
| Windows       | PortAudio     | Flutter | C++        |
| Android       | Oboe          | Flutter | C++        |
| iOS           | CoreAudio     | Flutter | C++        |

## Directory Structure

```
app/          Product layer: AppRuntime, persona, scheduler, tools, memory
core/         Agent framework: controller, context, policy, strategies
io/           IoDevice implementations: audio, ASR, TTS, VAD, probes
runtime/      Device bus: AgentRuntime, CoreDevice, ToolDispatchDevice, RouteTable
services/     Vendor clients: OpenAI, Baidu, ElevenLabs, memory, audit
ui/           Flutter app + C bridge
  bridge/     C API (shizuru_bridge.h/.cpp) — thin layer over AppRuntime
  lib/        Dart: FFI bindings, providers, screens, widgets
examples/     Runnable C++ examples (voice_agent, tool_call, etc.)
tests/        Unit, property-based, integration tests
utils/        Shared utilities (async logger)
```

## Key Design Decisions

### AgentRuntime is a pure device bus
Zero business logic. Registers devices, manages routes, dispatches frames, controls lifecycle. Session assembly (creating CoreDevice, wiring routes, registering tools) lives in `app/assembly/AppRuntime`.

### ToolDispatchDevice is a DMA controller
Lives in `runtime/`, not `io/`. Bridges the agent reasoning loop with external capabilities. Tool functions may have side effects (writing to scheduler, database) — this is the DMA layer's implementation detail, not the CPU's concern.

### Strategies are pluggable
ObservationFilter, ObservationAggregator, TtsSegmentStrategy, ResponseFilter are injected via constructor. Defaults are used when null. Strategies may own their own LlmClient for classification.

### Core as the sole decision center
All semantic decisions are made inside `core/` (Controller + strategies). IO devices do not make semantic judgments. The only exception is ToolDispatchDevice, which dispatches tool calls but does not decide whether to call them.

### Audio devices require explicit start
Registered with `DeviceOptions{.auto_start = false}`. Started manually by the bridge after platform permissions are granted.

## Services Directory Layout

```
services/
├── llm/openai/       → OpenAiClient (HTTP + SSE streaming)
├── asr/baidu/        → BaiduAsrClient
├── tts/baidu/        → BaiduTtsClient
├── tts/elevenlabs/   → ElevenLabsClient
├── utils/baidu/      → BaiduTokenManager
├── memory/           → InMemoryStore (header-only)
└── audit/            → LogAuditSink (header-only)
```

## Known Issues / TODO

- **Internal event type**: LLM cannot distinguish user input from system events (reminders, followups) in the context window. Need `kInternalEvent` MemoryEntryType mapped to `role: "system"` in conversation history.
- **Turn-trigger filter is temporarily disabled**: Controller currently bypasses semantic turn-trigger filtering and treats all meaningful observations as respond-now. Re-enable only after there is a robust, cancellable classifier path with explicit shutdown semantics.
- **Persistence is the current top priority**: Introduce the SQLite-backed MemoryStore and store real session/user data before adding more behavioral complexity.
- **Interaction meters are deferred**: Add session-scoped meters (for example reply threshold, text input rate, speech rate, emotional pressure) after persistence is in place, so dialogue policy can consume stable signal snapshots instead of ad-hoc heuristics.
- **MaybeSummarize is a stub**: does not actually call LLM for summarization.
- **Markdown stripping**: LLM sometimes outputs markdown despite prompt instructions.
