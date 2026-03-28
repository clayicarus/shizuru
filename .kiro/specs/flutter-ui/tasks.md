# Implementation Plan: Flutter UI (Phase 8)

## Overview

Bottom-up order: C API first, then FFI bridge, then state management, then UI.
Each task is independently testable. macOS is the primary target throughout.

## Tasks

- [x] 1. C++ shared library: `ui/bridge/`
  - Create `ui/bridge/shizuru_bridge.h` with all `extern "C"` declarations
    (see design.md C API section).
  - Create `ui/bridge/shizuru_bridge.cpp`:
    - Define `ShizuruContext` struct.
    - `shizuru_create`: parse config JSON (nlohmann/json or hand-rolled),
      construct `AgentRuntime` with the same device topology as
      `voice_agent.cpp` (capture, vad, asr_flush, asr, tts, playout, dumps).
      Wire all DMA routes. Register `OnOutput` callback that: for
      `is_partial=false` feeds full text to `tts_ptr->OnInput`; for both
      partial and final fires `output_cb(text, is_partial, user_data)` so
      Dart receives every token chunk. Keep raw pointers to `capture` and
      `tts` devices.
    - `shizuru_start`: call `runtime->StartSession()`.
    - `shizuru_destroy`: call `runtime->Shutdown()`, delete context.
    - `shizuru_send_message`: call `runtime->SendMessage(text)`.
    - `shizuru_get_state`: return `static_cast<int32_t>(runtime->GetState())`.
    - `shizuru_set_output_callback` / `shizuru_set_state_callback`: store in
      context under `cb_mutex`.
    - `shizuru_start_capture` / `shizuru_stop_capture`: call
      `capture->Start()` / `capture->Stop()` guarded by `capture_running`.
    - `shizuru_set_audio_level_callback`: hook into `EnergyVadDevice` RMS
      output (add a `LogDevice`-style probe or extend `EnergyVadDevice` API
      if needed).
  - Create `ui/bridge/CMakeLists.txt` as a `SHARED` library target
    `shizuru_bridge` linking all required device libraries.
  - Add `add_subdirectory(ui/bridge)` to root `CMakeLists.txt`.
  - _Requirements: 1.1–1.6, 2.1–2.6, 3.1–3.5_

- [x] 2. Flutter app scaffolding
  - Run `flutter create --platforms=macos,linux,windows ui` to generate the
    project skeleton.
  - Add dependencies to `pubspec.yaml`: `provider`, `shared_preferences`,
    `path_provider`.
  - Delete generated counter app boilerplate from `lib/main.dart`.
  - _Requirements: 9.1, 9.5_

- [x] 3. Dart: `AgentState` enum and `BridgeConfig`
  - Create `ui/lib/bridge/agent_state.dart`: `AgentState` enum with ordinals
    matching `core::State` (idle=0 … terminated=7). Add `displayName` getter.
  - Create `ui/lib/bridge/bridge_config.dart`: `BridgeConfig` class with all
    fields from design.md. Implement `toJson()` and `BridgeConfig.defaults()`.
  - _Requirements: 4.4, 5.1–5.4_

- [x] 4. Dart: FFI bindings (`shizuru_ffi.dart`)
  - Create `ui/lib/bridge/shizuru_ffi.dart`.
  - Load shared library by platform name in `ShizuruBridge._load()`.
  - Bind all C functions using `ffi.NativeFunction` lookups.
  - Implement `ShizuruBridge.create(BridgeConfig)` factory: serialize config
    to JSON, call `shizuru_create`, throw on null handle.
  - Implement `start()`, `destroy()`, `sendMessage()`, `getState()`.
  - Implement `onOutput(void Function(String text, bool isPartial))` and
    `onStateChange(callback)` using `NativeCallable.listener` to dispatch to
    the Flutter main isolate.
  - Implement `startCapture()`, `stopCapture()`, `onAudioLevel(callback)`.
  - _Requirements: 4.1–4.6_

- [x] 5. Dart: Providers
  - Create `ui/lib/providers/agent_provider.dart`:
    - Holds `ShizuruBridge? _bridge` and `AgentState state`.
    - `initialize(BridgeConfig)`: creates bridge, calls `start()`, registers
      callbacks.
    - `sendMessage(String)`, `toggleCapture()`.
    - `dispose()`: calls `bridge.destroy()`.
  - Create `ui/lib/providers/conversation_provider.dart`:
    - `List<ConversationMessage> messages`.
    - `void addUserMessage(String text)`.
    - `void onOutputChunk(String text, bool isPartial)`: on first partial
      chunk, append a new `ConversationMessage(isStreaming: true)`; on
      subsequent partials, update its `text` in place; on `isPartial=false`,
      set `isStreaming=false` to finalize the bubble.
    - `ScrollController` with auto-scroll on new message and on each partial
      chunk.
  - _Requirements: 6.1, 6.2, 6.5, 6.6, 6.9, 6.10_

- [x] 6. Dart: Settings screen
  - Create `ui/lib/screens/settings_screen.dart`.
  - `TextEditingController` for each config field.
  - Load saved values from `shared_preferences` on init.
  - Save button: validate required fields, persist, call
    `AgentProvider.initialize(newConfig)`.
  - _Requirements: 8.1–8.6_

- [x] 7. Dart: Conversation screen widgets
  - Create `ui/lib/widgets/message_bubble.dart`: role-colored bubble with
    timestamp. Shows animated dots when `isStreaming == true` (thinking
    indicator before first token) and renders growing text during streaming.
  - Create `ui/lib/widgets/waveform_bar.dart`: `AnimatedContainer` width
    driven by `AgentProvider.audioLevel`. Flat when not listening.
  - Create `ui/lib/widgets/state_indicator.dart`: colored dot with label,
    pulse animation for `listening`, spin for `thinking`/`acting`.
  - Create `ui/lib/widgets/debug_panel.dart`: `EndDrawer` with state label,
    transition log `ListView`, tool call log, clear button.
  - _Requirements: 6.3, 6.4, 6.7, 7.1–7.7_

- [x] 8. Dart: Conversation screen and app entry
  - Create `ui/lib/screens/conversation_screen.dart`:
    - `Scaffold` with `AppBar` (state indicator, debug toggle, settings icon).
    - `ListView.builder` consuming `ConversationProvider.messages`.
    - Waveform bar above input row.
    - `TextField` + send button + mic toggle button.
    - `EndDrawer` wired to `DebugPanel`.
  - Update `ui/lib/main.dart`:
    - `MultiProvider` with `AgentProvider` and `ConversationProvider`.
    - On startup: load config from `shared_preferences`; if complete, call
      `AgentProvider.initialize`; else push `SettingsScreen`.
  - _Requirements: 6.1–6.8, 8.1_

- [x] 9. macOS build integration
  - Add a `post_build` CMake rule (or `Podfile` script) that copies
    `libshizuru.dylib` into `ui/macos/Runner/Frameworks/`.
  - Verify `flutter build macos` produces a runnable `.app`.
  - Verify the app loads the shared library and starts the agent without
    crashing on a machine with valid API keys.
  - _Requirements: 9.2–9.4_

## Notes

- Task 1 (C++ bridge) can be built and smoke-tested with a small C test
  program before Flutter is involved.
- Tasks 3–5 (Dart layer) can be developed with a mock bridge before the
  real shared library is ready.
- Task 9 (macOS integration) is the final integration step; do it last.
- Token usage and route table visualization are deferred (see design.md).
- The `shizuru_set_audio_level_callback` implementation in task 1 may require
  a minor extension to `EnergyVadDevice` to expose per-frame RMS — evaluate
  during implementation and add a sub-task if needed.
