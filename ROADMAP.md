# Roadmap

## Phase 1 — Agent Framework (Done)

- [x] Controller: state machine with planning, routing, retry, stop conditions
- [x] Context strategy: prompt window management, token budget
- [x] Policy layer: permission checks and audit before every tool call
- [x] Session: full agent loop (observe → context → LLM → route → act → repeat)
- [x] OpenAI-compatible LLM client: HTTP + SSE streaming
- [x] Tool dispatcher and registry
- [x] In-memory store, log audit sink

## Phase 2 — Runtime IO Redesign (Done)

- [x] `IoDevice` abstract interface: typed input/output ports, `DataFrame` packets
- [x] `RouteTable`: source→destination port routing, control plane + DMA paths
- [x] `CoreDevice`: `IoDevice` adapter wrapping `AgentSession`
- [x] `AgentRuntime`: device bus — zero data transformation, pure frame routing
- [x] Full test suite: unit + property-based tests for runtime, controller, context, policy
- [x] `InitLogger` idempotency fix

## Phase 3 — Services Restructure (Done)

- [x] Reorganize `services/` into `services/<module>/<vendor>` layout
- [x] `services/llm/openai/` → `shizuru_llm_openai`
- [x] `services/asr/baidu/` → `shizuru_asr_baidu`
- [x] `services/tts/baidu/` → `shizuru_tts_baidu`
- [x] `services/tts/elevenlabs/` → `shizuru_tts_elevenlabs`
- [x] `services/utils/baidu/` → `shizuru_baidu_utils` (shared token manager)

## Phase 4 — Voice Pipeline (Done)

- [x] `AudioCaptureDevice` / `AudioPlayoutDevice`: PortAudio-backed `IoDevice` wrappers
- [x] `EnergyVadDevice`: energy-based VAD with sliding window RMS max-filter, pre-roll buffering, and audio gating — all in one `IoDevice`
- [x] `VadEventDevice`: fires a callback on configurable VAD events (e.g. `speech_end` → `asr.Flush()`)
- [x] `BaiduAsrDevice`: wraps `BaiduAsrClient` as an `IoDevice` (audio_in → text_out)
- [x] `BaiduTtsDevice`: wraps `BaiduTtsClient` as an `IoDevice` (text_in → audio_out)
- [x] `ElevenLabsTtsDevice`: wraps `ElevenLabsClient` as an `IoDevice`
- [x] `LogDevice` / `PcmDumpDevice`: observability probes (`shizuru_io_probe`)
- [x] DMA routes: capture → VAD → ASR → CoreDevice, CoreDevice → TTS → playout
- [x] VAD unit tests: 32 tests covering state machine, audio gating, pre-roll, sliding window
- [x] `asr_tts_echo_pipeline` example: full voice echo pipeline without LLM
- [x] `voice_agent` example: full voice agent (VAD + ASR + LLM + TTS)

## Phase 5 — Platform Audio Backends

- [ ] Android: Oboe backend (`io/audio/audio_device/oboe/`)
- [ ] iOS: CoreAudio backend (`io/audio/audio_device/core_audio/`)
- [ ] Windows: WASAPI backend (`io/audio/audio_device/wasapi/`)

## Phase 6 — Flutter UI

- [ ] dart:ffi bridge: expose `AgentRuntime` as a C API, bind from Dart
- [ ] Conversation view: message history, input field, audio waveform indicator
- [ ] Debug panel: state machine status, token usage, tool call log, route table view
- [ ] Cross-platform Flutter app scaffolding (desktop + mobile)

## Phase 7 — Production Hardening

- [ ] Config loader: `RuntimeConfig` from JSON/YAML file
- [ ] Persistent `MemoryStore`: SQLite-backed, survives restarts
- [ ] Token counting: tiktoken or equivalent for accurate budget management
- [ ] Human-in-the-loop approval flow (`PolicyLayer::ResolveApproval`)
- [ ] Built-in tools: filesystem read/write, HTTP fetch, code runner
- [ ] CI: GitHub Actions on macOS, Linux, Windows
- [ ] VAD: upgrade to WebRTC VAD or Silero for production accuracy
- [ ] Control plane: interrupt and reroute commands from controller to voice devices
