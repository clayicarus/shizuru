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

## Phase 5 — Thread Safety + Architecture Hardening (In Progress)

- [x] **T1-1** `AgentRuntime::DispatchFrame`: add `shared_mutex` to protect `devices_` and `route_table_` from concurrent access during `Shutdown`
- [x] **T1-2** `BaiduAsrDevice::Flush()`: remove blocking `join` from PortAudio callback thread; introduce internal worker queue
- [x] **T1-3** `ElevenLabsTtsDevice::OnInput`: remove blocking `join` from `Controller::loop_thread_`; post to internal queue
- [x] **T1-4** `CoreDevice::active_`: change `bool` to `std::atomic<bool>`
- [x] **T1-5** `Controller` callbacks: guard `OnResponse`/`OnTransition`/`OnDiagnostic` registration with a mutex or pre-`Start()` assertion
- [x] **T1-6** `AudioPlayoutDevice`: remove debug `static fopen`/`fwrite` from production code path
- [ ] **T1-7** `IoExecutor`: introduce a shared thread pool in `AgentRuntime` for network I/O tasks (ASR transcribe, TTS synthesize). Device-owned worker threads are replaced by injection of an `Executor&`; audio-path devices (capture, VAD, playout) are unaffected. Enables future migration to a lock-free MPSC queue without changing device code.
- [ ] **T2-1** `Controller`: remove `IoBridge` dependency; `HandleActing` emits `action_out` DataFrame and suspends in `kActing` until `kToolResult` observation arrives
- [ ] **T2-2** `CoreDevice`: remove `InterceptingIoBridge`; move `action_out` emit into `Controller`
- [ ] **T2-3** Add `ToolDispatchDevice`: `IoDevice` that executes tools from `ToolRegistry` and returns results as DataFrames
- [ ] **T2-4** Wire `core:action_out → tool_dispatch:action_in` and `tool_dispatch:result_out → core:tool_result_in` in `AgentRuntime`
- [ ] **T3-1** Add `control_out` port to `CoreDevice`; `Controller` emits control frames on interrupt and response delivery
- [ ] **T3-2** Define control frame protocol (`control/cancel`, `control/flush`) in a shared header
- [ ] **T3-3** Add `control_in` port to `ElevenLabsTtsDevice`, `AudioPlayoutDevice`, `BaiduAsrDevice`
- [ ] **T3-4** Remove direct `asr_ptr->Flush()` from `VadEventDevice`; route all control through `CoreDevice`

## Phase 6 — Audio Quality: 3A Processing

Desktop platforms (PortAudio) have no hardware 3A. Mobile platforms (Oboe, CoreAudio) expose hardware 3A which is sufficient at 16 kHz for both ASR input and TTS playout — no software processing needed there.

- [ ] **AEC** (Acoustic Echo Cancellation): software implementation for desktop; cancels TTS playout from the capture signal so the ASR does not transcribe the agent's own voice. Implemented as an `IoDevice` (`io/audio/aec/`) inserted between capture and VAD.
- [ ] **ANS** (Ambient Noise Suppression): software implementation for desktop; reduces background noise before ASR. Implemented as an `IoDevice` (`io/audio/ans/`) inserted between capture (or AEC output) and VAD.
- [ ] **AGC** (Automatic Gain Control): software implementation for desktop; normalizes capture level to keep ASR input within a consistent amplitude range. Implemented as an `IoDevice` (`io/audio/agc/`) in the same capture chain.
- [ ] Mobile: enable hardware 3A via Oboe (`AAudioStream` / `AudioEffect`) and CoreAudio session category flags — no additional `IoDevice` needed on those platforms.
- [ ] CMake: gate software 3A targets on `NOT (ANDROID OR IOS)`; mobile builds skip the `io/audio/aec`, `io/audio/ans`, `io/audio/agc` subdirectories entirely.
- [ ] Candidate library: [WebRTC AudioProcessing Module](https://chromium.googlesource.com/external/webrtc/) (APM) — provides AEC3, NS, AGC2 in a single C++ library, well-tested at 16 kHz.

## Phase 7 — Platform Audio Backends

- [ ] Android: Oboe backend (`io/audio/audio_device/oboe/`) with hardware 3A enabled
- [ ] iOS: CoreAudio backend (`io/audio/audio_device/core_audio/`) with hardware 3A via AVAudioSession
- [ ] Windows: WASAPI backend (`io/audio/audio_device/wasapi/`)

## Phase 8 — Flutter UI

- [ ] dart:ffi bridge: expose `AgentRuntime` as a C API, bind from Dart
- [ ] Conversation view: message history, input field, audio waveform indicator
- [ ] Debug panel: state machine status, token usage, tool call log, route table view
- [ ] Cross-platform Flutter app scaffolding (desktop + mobile)

## Phase 9 — Production Hardening

- [ ] Config loader: `RuntimeConfig` from JSON/YAML file
- [ ] Persistent `MemoryStore`: SQLite-backed, survives restarts
- [ ] Token counting: tiktoken or equivalent for accurate budget management
- [ ] Human-in-the-loop approval flow (`PolicyLayer::ResolveApproval`)
- [ ] Built-in tools: filesystem read/write, HTTP fetch, code runner
- [ ] CI: GitHub Actions on macOS, Linux, Windows
- [ ] VAD: upgrade to WebRTC VAD or Silero for production accuracy
- [ ] Control plane: interrupt and reroute commands from controller to voice devices
