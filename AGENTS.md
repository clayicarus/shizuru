# Shizuru — Agent Guidelines

All markdown documents must be written in English.

## Development Philosophy

This project is in its early stages. Always solve problems directly — no workarounds, no shortcuts, no deferred hacks. If something is broken, fix the root cause.

## Technology Stack

- Core runtime (agent framework + audio): C++17
- UI layer: Flutter (Dart), communicates with C++ core via dart:ffi
- LLM integration: OpenAI compatible API (HTTP + SSE streaming)
- Build system: CMake (C++ core), Flutter toolchain (UI)
- All AI model calls use API-based invocation, no local model inference

## Cross-Platform Strategy

The C++ core is the single shared codebase across all platforms.
Platform-specific code is isolated behind abstract interfaces (e.g., audio backends).

| Platform        | Audio Backend     | UI            | Agent Core |
|-----------------|-------------------|---------------|------------|
| macOS / Linux   | PortAudio         | Flutter       | C++        |
| Windows         | PortAudio / WASAPI| Flutter       | C++        |
| Android         | Oboe              | Flutter       | C++        |
| iOS             | CoreAudio         | Flutter       | C++        |

```
┌─────────────────────────────────────────┐
│              UI Layer (Flutter)          │
│  Dart: conversation UI, debug panel     │
└──────────────┬──────────────────────────┘
               │ dart:ffi
┌──────────────▼──────────────────────────┐
│         C++ Core (all platforms)        │
│  core/    controller, context, policy   │
│  services/ LLM, ASR, TTS, tools, memory │
│  io/      IoDevice abstraction + audio  │
│  runtime/ device bus, routing, session  │
└─────────────────────────────────────────┘
```

## Agent Architecture (OS-Inspired)

This architecture models an agent system similarly to an operating system:
- LLM as the reasoning core (CPU-like component)
- State machine as control flow and lifecycle manager
- IO module as the external interaction boundary
  - IO.Action for tool execution and side-effect operations
  - IO.Observation for input intake and result normalization
- Context strategy as memory and prompt orchestration
- Permission and policy layer as built-in security boundary

```mermaid
flowchart TB
    U[User / External Environment] <--> IOO[IO.Observation<br/>Input Intake, Normalization, Feedback]
    IOO --> C[Context Strategy<br/>Memory, Retrieval, Prompt Orchestration]
    C --> L[LLM Core<br/>HTTP + SSE Streaming Client]
    L --> S[State Machine / Controller<br/>Planning, Routing, Retry, Stop Conditions]
    S --> IOA[IO.Action<br/>APIs, DB, Filesystem, Browser, Code Runner]
    IOA --> IOO

    IOM[IO Module]
    IOA -. part of .-> IOM
    IOO -. part of .-> IOM

    P[Permission & Policy Layer<br/>RBAC/ABAC, Least Privilege, Approval Gates, Audit Logs]
    P -. governs .-> S
    P -. governs .-> IOA
    P -. filters .-> IOO

    G[Guardrails & Governance<br/>Budget, Safety Rules, Compliance Checks]
    G -. constrains .-> S
    G -. constrains .-> L
```

### Runtime Loop

1. Receive new user or environment input through IO.Observation.
2. Normalize and inject relevant data into context.
3. Let the LLM generate the next action candidate (via OpenAI compatible API).
4. Let the controller decide route: answer directly, call IO.Action, or continue planning.
5. Execute IO actions under permission and policy checks.
6. Feed execution results back through IO.Observation.
7. Repeat until stop condition is met, then return final response.

## Runtime IO Architecture (Implemented)

The runtime is built around a device bus model. All components — including the agent session itself — are `IoDevice` instances connected by a `RouteTable`.

### Key abstractions

- `IoDevice` (`io/io_device.h`) — abstract interface with typed input/output ports. Every component that produces or consumes data implements this.
- `DataFrame` (`io/data_frame.h`) — typed data packet passed between devices. Carries a MIME-like `type` field (e.g., `"text/plain"`, `"audio/pcm"`, `"action/tool_call"`).
- `RouteTable` (`runtime/route_table.h`) — maps `PortAddress{device_id, port_name}` → list of destination `PortAddress`. Supports two path types:
  - Control plane (`requires_control_plane = true`): routed through the runtime dispatch loop
  - DMA path (`requires_control_plane = false`): frame delivered directly, bypasses LLM/controller
- `CoreDevice` (`runtime/core_device.h`) — `IoDevice` adapter that wraps `AgentSession`. Translates `DataFrame` ↔ core types (`Observation`, `ActionCandidate`).
- `AgentRuntime` (`runtime/agent_runtime.h`) — device bus. Owns the device registry and route table. Zero data transformation — pure lifecycle management and frame routing.

### Invariants to preserve

- `AgentRuntime::DispatchFrame` must never transform frame data. It only routes.
- `CoreDevice` is the only place that translates between `DataFrame` and core types.
- New voice/audio components must be implemented as `IoDevice` and registered via `RegisterDevice` + `AddRoute`. Do not add audio logic directly to `AgentRuntime` or `CoreDevice`.
- DMA routes (`requires_control_plane = false`) must not involve the LLM or controller in their data path.
- `RouteTable` is the single source of truth for all data flow topology. Do not hardcode device-to-device calls.

## Voice Conversation Architecture (Decoupled Design)

The project is built around two independent but coordinated cores:
- Voice System Core: `capture`, `vad`, `asr`, `tts`, `playout` (C++, platform-specific backends)
- Agent Framework Core: reasoning, control, context, policy, and IO orchestration (C++, platform-independent)

Design principle:
- The voice pipeline is not hard-wired into the agent logic.
- For the agent, voice capabilities are IO devices registered in the runtime.
- Whether `capture` data should enter `vad` is a routing decision (RouteTable), not a fixed pipeline rule.
- When the agent wants to provide voice feedback, it invokes `tts` through IO.Action.

To preserve low latency, split runtime communication into:
- Control Plane: low-frequency decisions and commands (start, stop, route, interrupt)
- Data Plane: high-frequency audio flow (DMA path — `requires_control_plane = false`)

```mermaid
flowchart LR
    subgraph AF[Agent Framework Core - C++]
      L[LLM Client<br/>HTTP + SSE]
      S[State Machine / Controller]
      C[Context Strategy]
      P[Policy & Permissions]
      L --> S
      C --> L
      P -. governs .-> S
    end

    subgraph VS[Voice System Core - C++]
      CAP[Capture]
      VAD[VAD]
      ASR[ASR]
      TTS[TTS]
      PLAY[Playout]
    end

    subgraph CP[Control Plane]
      CMD[Commands / Routing / Interrupts]
    end

    subgraph DP[Data Plane]
      AIN[Audio Stream In]
      AOUT[Audio Stream Out]
    end

    subgraph UI[Flutter UI]
      CONV[Conversation View]
      DBG[Debug Panel]
    end

    UI <--> |dart:ffi| S
    S --> CMD
    CMD --> CAP
    CMD --> VAD
    CMD --> ASR
    CMD --> TTS
    CMD --> PLAY

    CAP --> AIN --> VAD --> ASR --> C
    S --> TTS --> AOUT --> PLAY
```

### Practical Boundary Rules

1. Decision ownership stays in the controller.
2. Voice runtime owns real-time media processing and quality.
3. Agent runtime owns orchestration, permissions, auditing, and task policies.
4. Data plane should avoid token-by-token LLM involvement for streaming media.
5. Flutter UI communicates with C++ core via dart:ffi; no business logic in Dart.

### VAD Device Design

`EnergyVadDevice` combines VAD detection and audio gating into a single `IoDevice`:
- Sliding window RMS max-filter (configurable window size, default 10 frames ~200ms)
- Pre-roll buffer: replays the last N frames on `speech_start` so onset frames are not lost
- Emits confirmed speech frames only on `audio_out`
- Emits `speech_start` / `speech_active` / `speech_end` events on `vad_out` for observability

`VadEventDevice` is a generic event sink: fires a callback when a specified VAD event arrives. Used to trigger `asr.Flush()` on `speech_end` without coupling VAD to ASR directly.

## Services Directory Layout

Services contain only vendor **client** implementations. IoDevice wrappers live in `io/`.

```
services/
├── llm/
│   └── openai/          → shizuru_llm_openai
├── asr/
│   └── baidu/           → shizuru_asr_baidu        (BaiduAsrClient only)
├── tts/
│   ├── baidu/           → shizuru_tts_baidu         (BaiduTtsClient only)
│   └── elevenlabs/      → shizuru_tts_elevenlabs    (ElevenLabsClient only)
├── utils/
│   └── baidu/           → shizuru_baidu_utils  (BaiduConfig + BaiduTokenManager)
├── io/                  → shizuru_io_services   (ToolDispatcher, ToolRegistry)
└── memory/              (InMemoryStore — header-only)
```

IoDevice implementations that wrap service clients live under `io/`:

```
io/
├── audio/               → shizuru_audio  (AudioCaptureDevice, AudioPlayoutDevice)
│   └── audio_device/    → platform backends (PortAudio, Oboe, CoreAudio)
├── asr/
│   └── baidu/           → shizuru_asr_baidu_device  (BaiduAsrDevice)
├── tts/
│   ├── baidu/           → shizuru_tts_baidu_device  (BaiduTtsDevice)
│   └── elevenlabs/      → shizuru_tts_elevenlabs_device  (ElevenLabsTtsDevice)
├── vad/                 → shizuru_vad  (EnergyVadDevice, VadEventDevice)
└── probe/               → shizuru_io_probe  (LogDevice, PcmDumpDevice)
```

When adding a new vendor implementation:
- Create `services/<module>/<vendor>/` with its own `CMakeLists.txt` for the client
- Create `io/<module>/<vendor>/` with its own `CMakeLists.txt` for the IoDevice wrapper
- Add `add_subdirectory(<vendor>)` in the respective parent `CMakeLists.txt`
- Shared vendor utilities (e.g., auth tokens) go in `services/utils/<vendor>/`
- Do not add vendor source files directly to a parent CMakeLists target

## Audio Backend Strategy

Audio recorder and player are defined as abstract C++ interfaces.
Platform-specific implementations live in separate directories:

```
io/audio/audio_device/
├── audio_recorder.h          # abstract interface
├── audio_player.h            # abstract interface
├── audio_device.h            # facade (concrete, owns recorder + player)
├── sample_format.h           # SampleFormat enum (kInt16, kFloat32)
├── audio_buffer.h            # lock-free ring buffer (template, internal)
├── port_audio/               # desktop: macOS, Linux, Windows
├── oboe/                     # Android
└── core_audio/               # iOS
```

CMake selects the correct backend at build time based on target platform.
