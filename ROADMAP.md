# Roadmap

## Phase 1 — Agent Framework Hardening (Weeks 1–2)

The core agent loop is implemented. This phase closes the remaining gaps before building on top of it.

- [ ] `runtime/config_loader`: load `RuntimeConfig` from a JSON/YAML file instead of hardcoding
- [ ] `PolicyLayer::ResolveApproval`: implement human-in-the-loop approval flow
- [ ] Token counting: integrate tiktoken or equivalent for accurate context budget management
- [ ] Persistent `MemoryStore`: SQLite or file-based, survives process restarts
- [ ] Built-in tools: filesystem read/write, HTTP fetch, code runner (registered via `ToolRegistry`)

## Phase 2 — Voice Pipeline (Weeks 3–5)

The agent runtime already has hooks for VAD, STT, and TTS (`PassesVadGate`, `TranscribeAudioToText`, `MaybeSynthesizeAudio`). This phase wires in real implementations.

- [ ] VAD: voice activity detection tool (WebRTC VAD or Silero)
- [ ] ASR: speech-to-text tool (Whisper API or local engine)
- [ ] TTS: text-to-speech tool (OpenAI TTS API or local engine)
- [ ] Audio recorder: refactor from pull mode (polling ring buffer) to push/callback-driven mode
- [ ] Control Plane: command routing between Agent Framework Core and Voice System Core
- [ ] Data Plane: low-latency audio streaming path that bypasses the LLM loop

## Phase 3 — Mobile Platform Backends (Weeks 4–6, parallel with Phase 2)

These are independent of each other and can be developed in parallel.

- [ ] Android: Oboe backend (`io/audio/audio_device/oboe/`)
- [ ] iOS: CoreAudio backend (`io/audio/audio_device/core_audio/`)
- [ ] Windows: WASAPI backend (`io/audio/audio_device/wasapi/`)

## Phase 4 — Flutter UI (Weeks 6–8)

- [ ] dart:ffi bridge: expose C++ `AgentRuntime` as a C API, bind from Dart
- [ ] Conversation view: message history, input field, audio waveform indicator
- [ ] Debug panel: state machine status, token usage, tool call log
- [ ] Cross-platform Flutter app scaffolding (desktop + mobile)

## Phase 5 — CI and Integration Testing (ongoing)

- [ ] GitHub Actions: build + test on macOS, Linux, Windows
- [ ] Integration test: full `AgentRuntime` round-trip with a mock LLM server
- [ ] Voice pipeline end-to-end test: capture → VAD → ASR → agent → TTS → playout
