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

## Phase 5 — Thread Safety + Architecture Hardening (Done)

- [x] **T1-1** `AgentRuntime::DispatchFrame`: add `shared_mutex` to protect `devices_` and `route_table_` from concurrent access during `Shutdown`
- [x] **T1-2** `BaiduAsrDevice::Flush()`: remove blocking `join` from PortAudio callback thread; introduce internal worker queue
- [x] **T1-3** `ElevenLabsTtsDevice::OnInput`: remove blocking `join` from `Controller::loop_thread_`; post to internal queue
- [x] **T1-4** `CoreDevice::active_`: change `bool` to `std::atomic<bool>`
- [x] **T1-5** `Controller` callbacks: guard `OnResponse`/`OnTransition`/`OnDiagnostic` registration with a mutex or pre-`Start()` assertion
- [x] **T1-6** `AudioPlayoutDevice`: remove debug `static fopen`/`fwrite` from production code path
- [x] **T2-1** `Controller`: remove `IoBridge` dependency; `HandleActing` emits `action_out` DataFrame and suspends in `kActing` until `kToolResult` observation arrives. `EmitFrameCallback` + `CancelCallback` replace `IoBridge`.
- [x] **T2-2** `CoreDevice`: remove `InterceptingIoBridge`; `action_out` emit handled directly by `Controller` via `EmitFrameCallback`
- [x] **T2-3** `ToolDispatchDevice`: `IoDevice` that executes tools from `ToolRegistry` and returns results as DataFrames on `result_out`
- [x] **T2-4** Wire `core:action_out → tool_dispatch:action_in` and `tool_dispatch:result_out → core:tool_result_in` in `AgentRuntime`
- [x] **T3-1** `CoreDevice`: add `control_out` port; emits `cancel` on `kInterrupt` (not on `kResponseDelivered`)
- [x] **T3-2** Control frame protocol (`cancel`, `flush`) defined in `io/control_frame.h`
- [x] **T3-3** `control_in` port added to `ElevenLabsTtsDevice`, `AudioPlayoutDevice`, `BaiduAsrDevice`
- [x] **T3-4** `VadEventDevice` refactored as a pass-through `IoDevice` with `vad_out` port; VAD events routed to `CoreDevice:vad_in` via `RouteTable`. `CoreDevice` emits `flush` on `speech_end` and `cancel` on `speech_start` (VAD interrupt).
- [x] **T1-7 (partial)** Device-owned worker threads retained; `IoExecutor` shared pool deferred to a later phase.
- [x] `AudioPlayer::Flush()`: new interface method — clears ring buffer without closing the PortAudio stream, used by `AudioPlayoutDevice` on `cancel` so subsequent TTS audio plays correctly
- [x] `ElevenLabsTtsDevice`: carry-byte buffer ensures every emitted PCM payload is even-sized (fixes int16 misalignment from odd-length HTTP chunks)
- [x] `PcmDumpDevice` probes added to `voice_agent`: `capture.pcm`, `vad_dump.pcm`, `playout_dump.pcm`
- [x] `MockLlmClient`: fixed mutex deadlock in `Cancel()` + added `WaitForCancel()` helper

## Phase 5.5 — Controller Strategies (Done)

- [x] Strategy interfaces: `ObservationFilter`, `TtsSegmentStrategy`, `ResponseFilter` in `core/strategies/`
- [x] Default implementations: `AcceptAllFilter`, `PunctuationSegmentStrategy`, `PassthroughFilter`, `StripThinkingFilter`
- [x] `LlmObservationFilter`: uses auxiliary LLM to classify ASR transcripts (yes/no)
- [x] Controller integration: strategies injected via constructor, used in `RunLoop`, `HandleThinking` (streaming), `HandleResponding`, `HandleInterrupt`
- [x] `RuntimeConfig`: strategy factory functions, called in `StartSession` to create per-session instances
- [x] Injection chain: `RuntimeConfig` → `AgentRuntime::StartSession` → `CoreDevice` → `AgentSession` → `Controller`
- [x] `voice_agent` example: wired with `LlmObservationFilter` + `PunctuationSegmentStrategy` + `StripThinkingFilter`
- [x] Unit + integration tests: 17 tests covering all strategies (unit tests for `PunctuationSegmentStrategy` and `StripThinkingFilter`, integration tests for `ObservationFilter`, `TtsSegmentStrategy`, `ResponseFilter` with mock LLM)
- [x] Fix: `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG` so `LOG_DEBUG` calls survive compilation

### TODO (deferred)

- [ ] **Observation aggregation**: `ObservationFilter` interface currently returns `bool` (pass/reject). To support buffering partial ASR transcripts until the user finishes speaking, the interface needs to change to `std::optional<Observation> Filter(const Observation&)` — allowing the filter to accumulate and merge multiple observations before forwarding a single complete one to the Controller.
- [ ] **Observation filter as independent session**: Consider promoting `ObservationFilter` from a synchronous in-Controller hook to an independent agent session with its own Controller, context, and LLM. This would allow context-aware filtering (e.g., "嗯" after a question is meaningful, "嗯" in silence is noise), avoid blocking the main Controller loop, and support stateful observation aggregation. The two sessions would need a coordination protocol for observation handoff and lifecycle management.
- [ ] **Logger separation**: examples currently share the global `shizuru` logger with core/runtime/io. When extracting the agent SDK, the library should use its own internal logger; applications should configure their own logger independently.
- [x] **TTS streaming think-tag filtering**: `TtsSegmentStrategy` currently accumulates raw streaming tokens. If the LLM produces `<think>` blocks during streaming, they will be buffered and potentially sent to TTS. The strategy should strip thinking tags from the token stream before accumulation.
- [ ] **Markdown stripping in ResponseFilter**: LLM sometimes outputs markdown formatting (e.g., `**bold**`, `# heading`) despite system prompt instructions. Add a markdown-cleaning step to `ResponseFilter` for voice output scenarios.
- [x] **Tool call state notification**: When LLM returns tool_calls (not content), streaming produces no tokens and TTS is silent. Expose a callback or state event so the UI/voice layer can provide feedback (e.g., "正在查询...") during tool execution.
- [ ] **Runtime thinking mode toggle**: `enable_thinking` is currently hardcoded to `true` in the bridge. Add a bridge API (`shizuru_set_thinking_enabled`) that dynamically updates `OpenAiConfig::enable_thinking` so the UI toggle can control it at runtime. Some models (e.g., Qwen3) may ignore this parameter and always think.
- [ ] **Structured rendering tag type system**: `<think>`, `<tool_call>`, `<tool_result>` tags are currently hardcoded strings in json_parser.cpp, controller.cpp, response_filter.h, and message_bubble.dart. These should be managed as a shared enum/constant set to avoid drift between C++ and Dart.
- [x] **Android SSL: replace BoringSSL hack with proper CA certificates**: Migrated from httplib to libcurl. On Android, BoringSSL is built via FetchContent and linked to curl. curl uses the system CA certificate directory (`/system/etc/security/cacerts`) for SSL verification — no bundled cert file needed. SSL verification is enabled on all platforms.
- [x] **Android SSL: clean up CMake BoringSSL integration**: Removed the fake `FindOpenSSL.cmake` shim and `shizuru_enable_https()` hack. BoringSSL is now properly integrated as curl's SSL backend on Android via standard CMake variables.

## Phase 6 — Audio Quality: 3A Processing

On macOS, `VoiceProcessingIO` Audio Unit provides system-level AEC + AGC + NS, but PortAudio opens a plain `RemoteIO` unit and does not activate it. A dedicated CoreAudio backend (`io/audio/audio_device/core_audio/`) using `VoiceProcessingIO` would expose hardware 3A on macOS without any software implementation. On Android (Oboe) and iOS (CoreAudio with AVAudioSession), hardware 3A is similarly available via platform APIs.

For Linux and Windows (PortAudio only), software 3A remains necessary.

- [ ] **macOS**: CoreAudio backend (`io/audio/audio_device/core_audio/`) using `kAudioUnitSubType_VoiceProcessingIO` — enables hardware AEC + AGC + NS, no software processing needed
- [ ] **AEC** (Acoustic Echo Cancellation): software implementation for Linux/Windows; cancels TTS playout from the capture signal so ASR does not transcribe the agent's own voice. `IoDevice` at `io/audio/aec/`, inserted between capture and VAD.
- [ ] **ANS** (Ambient Noise Suppression): software implementation for Linux/Windows. `IoDevice` at `io/audio/ans/`.
- [ ] **AGC** (Automatic Gain Control): software implementation for Linux/Windows. `IoDevice` at `io/audio/agc/`.
- [ ] Mobile: enable hardware 3A via Oboe (`AAudioStream` / `AudioEffect`) and CoreAudio AVAudioSession — no additional `IoDevice` needed on those platforms.
- [ ] CMake: gate software 3A targets on `NOT (APPLE OR ANDROID OR IOS)`; Apple and mobile builds use platform hardware 3A instead.
- [ ] Candidate library for software 3A: [WebRTC AudioProcessing Module](https://chromium.googlesource.com/external/webrtc/) (APM) — provides AEC3, NS, AGC2 in a single C++ library, well-tested at 16 kHz.

## Phase 7 — Platform Audio Backends

- [x] Android: Oboe backend (`io/audio/audio_device/oboe/`) — OboeRecorder + OboePlayer
- [ ] Android: audio routing control (speakerphone, earpiece, headset) should be encapsulated in the Oboe audio device layer via JNI, not in the Flutter UI. Currently speakerphone is toggled from Dart via MethodChannel as a workaround.
- [ ] **Device Group abstraction**: Introduce a device group concept in AgentRuntime to manage logical chains of devices (e.g., "voice_input" = capture + vad + asr, "voice_output" = tts + playout_dump + playout). StartGroup/StopGroup controls all devices in the chain. Needs reference counting for devices shared across groups — a device should only Stop when all groups referencing it have stopped. Currently managed ad-hoc in the bridge layer.
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

## Future Exploration

- [ ] **Android audio focus management**: Implement `AudioManager.OnAudioFocusChangeListener` and Oboe's `onErrorAfterClose` callback to automatically detect stream disconnection and recover. Currently requires manual restart via debug panel.
- [x] **Replace httplib with libcurl**: All HTTP clients (OpenAI LLM, ElevenLabs TTS, Baidu ASR/TTS/Token) migrated from cpp-httplib to libcurl. curl natively supports BoringSSL on Android and system SSL on desktop. The fake `FindOpenSSL.cmake` hack and all `enable_server_certificate_verification(false)` workarounds have been removed. SSL verification is now enabled on all platforms — Android uses the system CA cert directory (`/system/etc/security/cacerts`). httplib is retained only as a test dependency for mock HTTP servers.
- [ ] **VAD: evaluate open-source models**: Consider replacing energy-based VAD with model-based alternatives (Silero VAD, WebRTC VAD, or similar) for better accuracy in noisy environments and cross-language support.
- [ ] **On-device LLM inference** (low priority): Explore running lightweight models locally (e.g., via llama.cpp, MLC-LLM, or MediaPipe) for latency-sensitive tasks like observation filtering or TTS segmentation, reducing API dependency and network latency.
