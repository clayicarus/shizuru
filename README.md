# Shizuru

A cross-platform voice conversation agent with an architecture-first design.

## Technology Stack

- C++17: agent framework core + audio runtime (all platforms)
- Flutter: UI layer (desktop + mobile)
- OpenAI compatible API: all LLM calls via HTTP + SSE streaming
- CMake: C++ build system

## Project Structure

```
core/                   # Agent framework: controller, context, policy, session
  interfaces/           #   Abstract interfaces (LlmClient, IoBridge, etc.)
services/               # Concrete implementations of core interfaces
  llm/                  #   OpenAI compatible LLM client (HTTP + SSE)
  memory/               #   In-memory MemoryStore
  audit/                #   spdlog-based AuditSink
  io/                   #   Tool registry + dispatcher (IoBridge impl)
io/                     # IO module (namespace: shizuru::io)
  audio/                #   Audio subsystem (C++)
    audio_device/
      audio_recorder.h  #     Abstract recorder interface
      audio_player.h    #     Abstract player interface
      audio_device.h    #     Facade: StartRecording / StartPlayout
      sample_format.h   #     SampleFormat enum + BytesPerSample
      audio_buffer.h    #     Lock-free ring buffer (template, internal)
      port_audio/       #     PortAudio backend (desktop)
      oboe/             #     Oboe backend (Android, planned)
      core_audio/       #     CoreAudio backend (iOS, planned)
runtime/                # AgentRuntime: assembles all components, session lifecycle
tests/
  agent/                #   Unit + property tests for core modules
  services/             #   Unit tests for services (json_parser, tool_dispatcher)
ui/                     # Flutter app (planned)
```

## Core Architecture

The system uses a decoupled, OS-inspired design:
- Agent Framework Core: `LLM Client + Controller + Context Strategy + Policy`
- Voice System Core: `capture + vad + asr + tts + playout`

Voice capabilities are IO devices/services for the agent, not hard-coded pipeline steps.

## Design Principles

1. Decouple voice pipeline from agent reasoning.
2. Keep decisions in the controller, not in device handlers.
3. Use policy and permission layers as first-class runtime boundaries.
4. Separate control plane (routing, lifecycle) from data plane (audio streams).
5. C++ core is platform-independent; platform-specific code is behind abstract interfaces.
6. Flutter UI has no business logic; it communicates with C++ core via dart:ffi.

## Cross-Platform Support

| Platform      | Audio Backend      | UI      | Agent Core |
|---------------|--------------------|---------|------------|
| macOS / Linux | PortAudio          | Flutter | C++        |
| Windows       | PortAudio / WASAPI | Flutter | C++        |
| Android       | Oboe               | Flutter | C++        |
| iOS           | CoreAudio          | Flutter | C++        |

## Runtime Loop

1. IO.Observation receives user/environment input.
2. Context strategy prepares working context for the LLM.
3. LLM client calls OpenAI compatible API (streaming).
4. Controller chooses route: respond, call IO.Action, or continue planning.
5. Policy/permission checks are enforced before sensitive actions.
6. Voice/media outputs flow through data plane with low-latency constraints.
7. Results are observed, normalized, and fed back into the next cycle.

## TODO

### Build & Infrastructure
- [ ] Install OpenSSL and enable HTTPS support in `services/llm/CMakeLists.txt` (required for `https://api.openai.com`; currently only HTTP endpoints like Ollama work)
- [ ] Fix PortAudio CMake compatibility warning (`cmake_minimum_required < 3.10` deprecated in CMake 4.x) — upstream PortAudio issue, track for update
- [ ] Add Ninja generator support documentation for faster builds on Windows
- [ ] Add CI pipeline (GitHub Actions) for automated build + test

### Agent Framework
- [ ] `runtime/config_loader.h/.cpp` — Load `RuntimeConfig` from JSON/YAML file instead of hardcoding
- [ ] Persistent `MemoryStore` implementation (SQLite or file-based) to survive process restarts
- [ ] Implement `ResolveApproval` in `PolicyLayer` for interactive human-in-the-loop approval flow
- [ ] Add built-in tools: filesystem, HTTP fetch, code runner (register via `ToolRegistry`)
- [ ] Token counting integration (tiktoken or equivalent) for accurate context budget management
- [ ] Retry and exponential backoff for LLM API transient failures in `OpenAiClient`
- [ ] Rate limiting / token budget enforcement at the `OpenAiClient` level

### Voice System Core
- [ ] Audio: refactor recorder from pull mode (polling ring buffer) to push mode (callback-driven), relying on OS/hardware clock to minimize latency
- [ ] VAD (Voice Activity Detection) module
- [ ] ASR (Automatic Speech Recognition) integration — Whisper API or local engine
- [ ] TTS (Text-to-Speech) integration — OpenAI TTS API or local engine
- [ ] Control Plane: command routing between Agent Framework Core and Voice System Core
- [ ] Data Plane: low-latency audio streaming path (DMA-like, bypasses LLM loop)

### Platform Backends
- [ ] Oboe audio backend for Android (`io/audio/audio_device/oboe/`)
- [ ] CoreAudio backend for iOS (`io/audio/audio_device/core_audio/`)
- [ ] WASAPI backend option for Windows (`io/audio/audio_device/wasapi/`)

### UI Layer
- [ ] Flutter UI: conversation view + debug panel
- [ ] dart:ffi bridge between Flutter and C++ core
- [ ] Cross-platform Flutter app scaffolding (desktop + mobile)

### Testing
- [ ] Integration test: full `AgentRuntime` round-trip with mock LLM server
- [ ] `OpenAiClient` unit tests with local HTTP mock server
- [ ] Voice pipeline end-to-end test (capture → VAD → ASR → agent → TTS → playout)

## Building (C++ core)

```bash
cmake -B build -G Ninja
cmake --build build
```

## Security and Governance

- Least-privilege by default
- Capability-based tool access
- Approval gates for high-risk actions
- Full audit logs for decisions and executions
- Budget and safety guardrails for runtime control
