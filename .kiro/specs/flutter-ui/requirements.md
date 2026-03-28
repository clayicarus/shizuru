# Requirements: Flutter UI (Phase 8)

## Introduction

This feature adds a Flutter desktop UI to Shizuru. The UI communicates with the
C++ core exclusively via `dart:ffi`. No business logic lives in Dart — the C++
`AgentRuntime` remains the single source of truth for all agent state.

The target capability mirrors `examples/voice_agent.cpp`: a voice agent with
VAD-gated microphone input, Baidu ASR, OpenAI-compatible LLM, ElevenLabs TTS,
and PortAudio playout — all wired through the existing `IoDevice` / `RouteTable`
bus. The UI surfaces this as a conversation interface with a debug panel.

Initial target: macOS desktop. The same Flutter app will run on Linux and Windows
once their audio backends are complete (Phase 7).

---

## Glossary

- **C API**: A plain-C header (`ui/bridge/shizuru_bridge.h`) that exposes
  `AgentRuntime` operations as `extern "C"` functions. The only ABI boundary
  between Dart and C++.
- **FFI bridge**: The Dart-side binding (`ui/lib/bridge/`) that loads the shared
  library and wraps C API calls in type-safe Dart functions.
- **AgentRuntime**: `shizuru::runtime::AgentRuntime` — the C++ device bus that
  owns all voice and agent devices.
- **RuntimeConfig**: `shizuru::runtime::RuntimeConfig` — configuration bundle
  (LLM endpoint, API keys, controller limits, etc.).
- **State**: `shizuru::core::State` enum — `Idle`, `Listening`, `Thinking`,
  `Routing`, `Acting`, `Responding`, `Error`, `Terminated`.
- **ConversationMessage**: A single turn in the conversation (role + text +
  timestamp). Maintained in Dart; populated from C++ callbacks.
- **DebugPanel**: Secondary view showing real-time agent internals: current state,
  token usage, tool call log, active routes.
- **Waveform indicator**: A simple amplitude bar that reflects microphone energy
  during `Listening` state.
- **shared library**: The C++ core compiled as `libshizuru.dylib` (macOS),
  `libshizuru.so` (Linux), or `shizuru.dll` (Windows). Loaded by Flutter at
  runtime via `dart:ffi`.

---

## Requirements

### Requirement 1: C API — Lifecycle

**User Story:** As a Flutter developer, I want a stable plain-C API to create,
start, and destroy an `AgentRuntime` instance, so that Dart can manage the
agent lifecycle without depending on C++ headers.

#### Acceptance Criteria

1. THE C API SHALL expose `shizuru_create(config_json)` that parses a JSON string
   into `RuntimeConfig`, constructs an `AgentRuntime`, and returns an opaque
   `ShizuruHandle`.
2. THE C API SHALL expose `shizuru_start(handle)` that calls
   `AgentRuntime::StartSession()` and returns 0 on success or a negative error
   code on failure.
3. THE C API SHALL expose `shizuru_destroy(handle)` that calls
   `AgentRuntime::Shutdown()` and frees all resources.
4. IF `shizuru_create` receives a malformed JSON config, IT SHALL return a null
   handle and write a human-readable error string to an out-parameter.
5. ALL C API functions SHALL be declared in `ui/bridge/shizuru_bridge.h` with
   `extern "C"` linkage and no C++ types in their signatures.
6. THE shared library SHALL be built by a dedicated CMake target
   `shizuru_bridge` that links `shizuru_runtime` and all required device
   libraries.

---

### Requirement 2: C API — Messaging and State

**User Story:** As a Flutter developer, I want to send text messages and query
agent state through the C API, so that the UI can drive the agent and reflect
its current status.

#### Acceptance Criteria

1. THE C API SHALL expose `shizuru_send_message(handle, text)` that calls
   `AgentRuntime::SendMessage(text)`.
2. THE C API SHALL expose `shizuru_get_state(handle)` that returns the current
   `core::State` as an `int32_t` matching the enum ordinal.
3. THE C API SHALL expose `shizuru_set_output_callback(handle, callback, user_data)`
   where `callback` is `void(*)(const char* text, int32_t is_partial, void* user_data)`.
   IT SHALL be called on every `AgentRuntime::OutputCallback` invocation, with
   `is_partial=1` for streaming token chunks and `is_partial=0` for the final
   complete response. This maps directly to `RuntimeOutput::is_partial`.
4. THE C API SHALL expose `shizuru_set_state_callback(handle, callback, user_data)`
   where `callback` is `void(*)(int32_t state, void* user_data)`. IT SHALL be
   called whenever `core::State` changes.
5. ALL callbacks SHALL be invoked on a dedicated event thread, never on the
   PortAudio or LLM streaming thread, to avoid blocking real-time audio paths.
6. THE `shizuru_get_state` function SHALL be safe to call from any thread.

---

### Requirement 3: C API — Voice Control

**User Story:** As a Flutter developer, I want to start and stop microphone
capture through the C API, so that the UI can provide a push-to-talk or
always-on toggle.

#### Acceptance Criteria

1. THE C API SHALL expose `shizuru_start_capture(handle)` that starts the
   `AudioCaptureDevice` (begins microphone input).
2. THE C API SHALL expose `shizuru_stop_capture(handle)` that stops the
   `AudioCaptureDevice` (mutes microphone input).
3. THE C API SHALL expose `shizuru_set_audio_level_callback(handle, callback, user_data)`
   where `callback` is `void(*)(float rms, void* user_data)`. IT SHALL be called
   at approximately 20ms intervals with the current microphone RMS level during
   active capture, sourced from `EnergyVadDevice` output.
4. IF capture is already running when `shizuru_start_capture` is called, IT SHALL
   be a no-op and return 0.
5. IF capture is already stopped when `shizuru_stop_capture` is called, IT SHALL
   be a no-op and return 0.

---

### Requirement 4: FFI Bridge (Dart)

**User Story:** As a Flutter developer, I want a type-safe Dart wrapper around
the C API, so that UI code never calls raw FFI functions directly.

#### Acceptance Criteria

1. THE FFI bridge SHALL be implemented in `ui/lib/bridge/shizuru_ffi.dart` and
   expose a `ShizuruBridge` class with methods mirroring the C API.
2. THE `ShizuruBridge` class SHALL load the correct shared library name per
   platform: `libshizuru.dylib` (macOS), `libshizuru.so` (Linux),
   `shizuru.dll` (Windows).
3. THE `ShizuruBridge` class SHALL expose `start()`, `destroy()`,
   `sendMessage(String text)`, `getState()`,
   `onOutput(void Function(String text, bool isPartial))`,
   `onStateChange(void Function(AgentState))`, `startCapture()`, `stopCapture()`,
   and `onAudioLevel(void Function(double rms))`.
4. THE `AgentState` Dart enum SHALL mirror `core::State` ordinals exactly:
   `idle=0, listening=1, thinking=2, routing=3, acting=4, responding=5,
   error=6, terminated=7`.
5. ALL callbacks registered via the bridge SHALL be dispatched to the Flutter
   main isolate using `NativeCallable` or equivalent, so that UI widgets can
   call `setState` safely.
6. THE bridge SHALL expose a `factory ShizuruBridge.create(BridgeConfig config)`
   constructor that calls `shizuru_create` with a JSON-serialized config.

---

### Requirement 5: BridgeConfig (Dart)

**User Story:** As a Flutter developer, I want a Dart config object that maps
to `RuntimeConfig`, so that API keys and LLM settings can be provided from the
UI layer without hardcoding them in C++.

#### Acceptance Criteria

1. THE `BridgeConfig` Dart class SHALL include fields: `llmBaseUrl`, `llmApiPath`,
   `llmApiKey`, `llmModel`, `elevenLabsApiKey`, `baiduApiKey`, `baiduSecretKey`,
   `systemInstruction`, `maxTurns`.
2. THE `BridgeConfig` SHALL serialize to a JSON string compatible with the
   `shizuru_create` config parser.
3. THE app SHALL read API keys from environment variables or a local
   `.env`-style config file at startup; keys SHALL NOT be hardcoded in source.
4. THE `BridgeConfig` SHALL provide sensible defaults for `llmApiPath`
   (`/compatible-mode/v1/chat/completions`), `llmModel` (`qwen3-coder-next`),
   and `maxTurns` (100).

---

### Requirement 6: Conversation View

**User Story:** As a user, I want to see the conversation history and the
agent's current status in a clean interface, so that I can follow the dialogue
and understand what the agent is doing.

#### Acceptance Criteria

1. THE conversation view SHALL display a scrollable list of `ConversationMessage`
   items, each showing role (`user` / `assistant`), text content, and timestamp.
2. THE conversation view SHALL auto-scroll to the latest message when a new
   message is appended.
3. THE conversation view SHALL show a status indicator reflecting the current
   `AgentState`: a colored dot or label (`Listening`, `Thinking`, `Acting`, etc.).
4. THE conversation view SHALL show a waveform/amplitude bar that animates with
   microphone RMS level during `listening` state and is flat otherwise.
5. THE conversation view SHALL include a text input field and send button for
   typed messages, calling `ShizuruBridge.sendMessage`.
6. THE conversation view SHALL include a microphone toggle button that calls
   `startCapture` / `stopCapture`.
7. WHEN the agent is in `Thinking`, `Routing`, or `Acting` state, THE UI SHALL
   show a loading indicator (e.g., animated dots) next to the assistant bubble.
8. THE conversation view SHALL be the default/home screen of the app.
9. WHEN the agent is in `Responding` state, THE UI SHALL display an assistant
   message bubble that grows token-by-token as streaming chunks arrive
   (`is_partial=true`). The bubble SHALL be finalized (marked complete) when
   the final response (`is_partial=false`) is received.
10. WHILE a streaming response is in progress, THE UI SHALL auto-scroll to keep
    the bottom of the growing bubble visible.

---

### Requirement 7: Debug Panel

**User Story:** As a developer, I want a debug panel that shows agent internals
in real time, so that I can diagnose issues without reading log files.

#### Acceptance Criteria

1. THE debug panel SHALL be accessible via a toggle button or keyboard shortcut
   from the conversation view.
2. THE debug panel SHALL display the current `AgentState` as a text label,
   updated in real time via `onStateChange`.
3. THE debug panel SHALL display a running log of state transitions with
   timestamps (e.g., `14:32:01.123  Listening → Thinking`).
4. THE debug panel SHALL display a tool call log: each entry shows tool name,
   arguments (truncated to 120 chars), result status (success/failure), and
   latency in ms.
5. THE debug panel SHALL display current token usage if available (prompt tokens,
   completion tokens) — sourced from LLM response metadata when the C API
   exposes it.
6. THE debug panel SHALL be implemented as a side drawer or split-pane that
   overlays the conversation view without replacing it.
7. THE debug panel log SHALL be clearable via a "Clear" button.

---

### Requirement 8: App Scaffolding and Configuration Screen

**User Story:** As a user, I want a settings screen where I can enter API keys
and LLM endpoint, so that I can configure the agent without recompiling.

#### Acceptance Criteria

1. THE app SHALL show a configuration screen on first launch (or when config is
   missing) before starting the agent.
2. THE configuration screen SHALL have input fields for: LLM base URL, LLM API
   key, LLM model name, ElevenLabs API key, Baidu API key, Baidu secret key,
   and system instruction.
3. THE configuration screen SHALL persist settings to local storage
   (e.g., `shared_preferences` or a local JSON file).
4. THE configuration screen SHALL be accessible from the conversation view via
   a settings icon.
5. WHEN the user saves a new config, THE app SHALL call `shizuru_destroy` on
   the existing handle (if any) and `shizuru_create` + `shizuru_start` with the
   new config.
6. THE app SHALL validate that required fields (LLM API key, ElevenLabs API key,
   Baidu keys) are non-empty before allowing the agent to start.

---

### Requirement 9: Desktop Platform Targets

**User Story:** As a developer, I want the Flutter app to build and run on
macOS desktop first, with Linux and Windows following the same structure, so
that the UI is ready for all desktop platforms when their audio backends land.

#### Acceptance Criteria

1. THE Flutter app SHALL be scaffolded with macOS, Linux, and Windows desktop
   targets enabled.
2. THE CMake `shizuru_bridge` target SHALL produce the correct shared library
   artifact for each platform: `.dylib` (macOS), `.so` (Linux), `.dll` (Windows).
3. THE Flutter build SHALL locate the shared library via a relative path from
   the app bundle's `Frameworks/` directory (macOS) or alongside the executable
   (Linux/Windows).
4. THE app SHALL compile and run on macOS without modification to existing C++
   source files outside `ui/bridge/`.
5. THE Flutter app directory SHALL be `ui/` at the workspace root, with the
   standard Flutter project layout inside.
