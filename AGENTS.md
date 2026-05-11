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

### Unified Semantic Pipeline
The system uses a clear four-layer architecture:
- **Perception Layer**: Raw signal capture (audio, WebSocket, UI input)
- **Interpretation Layer**: Converts raw signals to `ConversationItem` (source-final semantic units)
- **Semantic Layer**: `ConversationItem`, `InvokeBatch`, `ControlSignal` — shared architecture boundary types
- **Reasoning Layer**: Core batching, LLM invocation, output interpretation, history, provider render

Key types:
- `core::ConversationItem` — the single semantic unit for all conversation events
- `core::InvokeBatch` — one LLM call's input (groups multiple ConversationItems)
- `core::ControlSignal` — control plane events (flush, cancel, interrupt, tool results)
- `core::ContentPart` — variant of TextPart, ImagePart, AudioPart, ToolCallPart, ToolResultPart

### AgentRuntime is a pure device bus
Zero business logic. Registers devices, manages routes, dispatches frames, controls lifecycle. Session assembly (creating CoreDevice, wiring routes, registering tools) lives in `app/assembly/AppRuntime`.

### ToolDispatchDevice emits ControlSignals only
Lives in `runtime/`, not `io/`. Executes tool calls and returns `ToolResultSignal` to Core. Core constructs the `ConversationItem(kToolResult)` for history — the tool executor never constructs semantic items (requirement 9.1-9.3).

### Interpreters output ConversationItem directly
- OneBot parser → `ConversationItem` (with actor, parts, mentions, reply_to)
- Flutter input adapter → `ConversationItem`
- ASR adapter → `ConversationItem` (only on final result; partials stay internal)
- Scheduler adapter → `ConversationItem(kSystemEvent)`

### Core is the sole batching decision maker
Whether to combine multiple ConversationItems into one LLM call is Core's decision. Source adapters must not do cross-actor aggregation (requirement 7.1-7.3).

### Provider payload is a terminal projection
Internal models are never collapsed into provider format. The `provider_render` module projects `InvokeBatch` + history into OpenAI messages JSON at the boundary.

### Strategies are pluggable
TtsSegmentStrategy, ResponseFilter are injected via constructor. Defaults are used when null.

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

- **OneBot 11 protocol reference**: https://github.com/botuniverse/onebot-11 — Event/API schema: https://api.luckylillia.com/doc-7202281

- **Integration compilation**: The unified pipeline refactoring introduced new types but several existing files (context_strategy.cpp, default_reducer.cpp, controller.cpp, session.cpp) still reference deleted types and need to be rewritten to use the new ConversationItem-based interfaces.
- **Turn-trigger filter is temporarily disabled**: Controller currently bypasses semantic turn-trigger filtering. Re-enable after the dialogue kernel is rebuilt with the new event types.
- **MaybeSummarize is a stub**: does not actually call LLM for summarization.
- **Markdown stripping**: LLM sometimes outputs markdown despite prompt instructions.
- **`<skip/>` response tag is hardcoded in onebot_agent example**: Should be formalized as a core ResponseFilter.
- **Dual output paths (route vs callback) need consolidation**: The route path should be the single delivery mechanism; callbacks should be for observability/UI only.
