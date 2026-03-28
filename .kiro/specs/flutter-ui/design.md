# Design: Flutter UI (Phase 8)

## Overview

The UI layer is a Flutter desktop app in `ui/`. It communicates with the C++
core through a thin plain-C shared library (`libshizuru`). No agent logic lives
in Dart — the C++ `AgentRuntime` owns everything.

```
┌─────────────────────────────────────────────┐
│  Flutter App  (ui/)                          │
│  ┌──────────────┐  ┌──────────────────────┐ │
│  │ Conversation │  │   Debug Panel        │ │
│  │ View         │  │   (drawer/split)     │ │
│  └──────┬───────┘  └──────────────────────┘ │
│         │  Dart providers / state           │
│  ┌──────▼───────────────────────────────┐   │
│  │  ShizuruBridge  (lib/bridge/)        │   │
│  │  dart:ffi  →  libshizuru.dylib       │   │
│  └──────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
               │  extern "C"
┌──────────────▼──────────────────────────────┐
│  ui/bridge/  (C++ shared library)           │
│  shizuru_bridge.h / shizuru_bridge.cpp      │
│  ┌──────────────────────────────────────┐   │
│  │  AgentRuntime  +  voice IoDevices    │   │
│  │  (same topology as voice_agent.cpp)  │   │
│  └──────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
```

---

## Directory Layout

```
ui/
├── bridge/                        ← C++ shared library (CMake target: shizuru_bridge)
│   ├── CMakeLists.txt
│   ├── shizuru_bridge.h           ← plain-C public API
│   └── shizuru_bridge.cpp         ← implementation
├── pubspec.yaml
├── lib/
│   ├── main.dart
│   ├── bridge/
│   │   ├── shizuru_ffi.dart       ← FFI bindings + ShizuruBridge class
│   │   ├── bridge_config.dart     ← BridgeConfig + JSON serialization
│   │   └── agent_state.dart       ← AgentState enum (mirrors core::State)
│   ├── providers/
│   │   ├── agent_provider.dart    ← ChangeNotifier: bridge lifecycle + state
│   │   └── conversation_provider.dart  ← message list, auto-scroll
│   ├── screens/
│   │   ├── conversation_screen.dart
│   │   └── settings_screen.dart
│   └── widgets/
│       ├── message_bubble.dart
│       ├── waveform_bar.dart
│       ├── state_indicator.dart
│       └── debug_panel.dart
└── macos/                         ← Flutter macOS runner (generated)
```

---

## C API Design (`ui/bridge/shizuru_bridge.h`)

```c
typedef void* ShizuruHandle;
typedef void (*ShizuruOutputCallback)(const char* text, int32_t is_partial, void* user_data);
typedef void (*ShizuruStateCallback)(int32_t state, void* user_data);
typedef void (*ShizuruAudioLevelCallback)(float rms, void* user_data);

// Lifecycle
ShizuruHandle shizuru_create(const char* config_json, char* error_buf, int error_buf_len);
int32_t       shizuru_start(ShizuruHandle handle);
void          shizuru_destroy(ShizuruHandle handle);

// Messaging
int32_t shizuru_send_message(ShizuruHandle handle, const char* text);
int32_t shizuru_get_state(ShizuruHandle handle);

// Callbacks
void shizuru_set_output_callback(ShizuruHandle handle,
                                 ShizuruOutputCallback cb, void* user_data);
void shizuru_set_state_callback(ShizuruHandle handle,
                                ShizuruStateCallback cb, void* user_data);

// Voice control
int32_t shizuru_start_capture(ShizuruHandle handle);
int32_t shizuru_stop_capture(ShizuruHandle handle);
void    shizuru_set_audio_level_callback(ShizuruHandle handle,
                                         ShizuruAudioLevelCallback cb,
                                         void* user_data);
```

### Config JSON schema (passed to `shizuru_create`)

```json
{
  "llm_base_url":        "https://dashscope.aliyuncs.com",
  "llm_api_path":        "/compatible-mode/v1/chat/completions",
  "llm_api_key":         "...",
  "llm_model":           "qwen3-coder-next",
  "elevenlabs_api_key":  "...",
  "baidu_api_key":       "...",
  "baidu_secret_key":    "...",
  "system_instruction":  "You are a helpful voice assistant...",
  "max_turns":           100
}
```

### Internal structure of `ShizuruHandle`

`shizuru_bridge.cpp` defines a `ShizuruContext` struct (heap-allocated, cast to
`void*`):

```cpp
struct ShizuruContext {
  runtime::AgentRuntime* runtime;
  io::AudioCaptureDevice* capture;   // non-owning, owned by runtime
  io::ElevenLabsTtsDevice* tts;      // non-owning
  std::atomic<bool> capture_running{false};

  // Callback state
  ShizuruOutputCallback output_cb = nullptr;
  void* output_user_data = nullptr;
  ShizuruStateCallback state_cb = nullptr;
  void* state_user_data = nullptr;
  ShizuruAudioLevelCallback audio_level_cb = nullptr;
  void* audio_level_user_data = nullptr;

  std::mutex cb_mutex;
};
```

The `AgentRuntime` is constructed with the same device topology as
`voice_agent.cpp`. The `OnOutput` callback feeds text to `tts_ptr->OnInput`
and also fires `output_cb` so Dart receives the text.

---

## Dart Layer Design

### State management: Provider

Two `ChangeNotifier` providers:

**`AgentProvider`** — owns the `ShizuruBridge` instance:
- `AgentState state` — updated via `onStateChange` callback
- `bool captureActive` — toggled by `startCapture` / `stopCapture`
- `double audioLevel` — updated via `onAudioLevel` at ~20ms intervals
- `void sendMessage(String text)` — delegates to bridge
- `void toggleCapture()` — calls `startCapture` or `stopCapture`

**`ConversationProvider`** — owns the message list:
- `List<ConversationMessage> messages`
- `void addUserMessage(String text)` — called before `sendMessage`
- `void onOutputChunk(String text, bool isPartial)` — called from `onOutput`
  callback; appends a new streaming bubble on first partial chunk, updates text
  in place on subsequent partials, finalizes on `isPartial=false`
- `ScrollController scrollController` — auto-scrolls on new message and on each
  partial chunk while streaming

### `ConversationMessage`

```dart
class ConversationMessage {
  final String role;       // 'user' | 'assistant'
  final String text;
  final DateTime timestamp;
  final bool isStreaming;  // true while is_partial tokens are arriving
}
```

Streaming flow: when the first `is_partial=true` chunk arrives, a new
`ConversationMessage` is appended with `isStreaming=true`. Subsequent partial
chunks update the `text` of that same message in place. When `is_partial=false`
arrives, `isStreaming` is set to `false` and the bubble is finalized.

### `AgentState` enum

```dart
enum AgentState {
  idle,        // 0
  listening,   // 1
  thinking,    // 2
  routing,     // 3
  acting,      // 4
  responding,  // 5
  error,       // 6
  terminated,  // 7
}
```

---

## UI Layout

### Conversation Screen

```
┌─────────────────────────────────────────────┐
│  [●] Shizuru          [debug] [settings]    │  ← AppBar
├─────────────────────────────────────────────┤
│                                             │
│   ┌─────────────────────────────────────┐  │
│   │  user  14:32:00                     │  │
│   │  Hello, what's the weather today?   │  │
│   └─────────────────────────────────────┘  │
│                                             │
│   ┌─────────────────────────────────────┐  │
│   │  assistant  14:32:02                │  │
│   │  I don't have real-time weather...  │  │
│   └─────────────────────────────────────┘  │
│                                             │
│   ┌─────────────────────────────────────┐  │
│   │  assistant  ●●●  (thinking)         │  │  ← loading indicator
│   └─────────────────────────────────────┘  │
│                                             │
├─────────────────────────────────────────────┤
│  [▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]  │  ← waveform bar (RMS)
│  [🎤]  Type a message...          [Send]   │  ← input row
└─────────────────────────────────────────────┘
```

State indicator dot colors:
- `idle` / `terminated` → grey
- `listening` → green (pulsing)
- `thinking` / `routing` → amber (spinning)
- `acting` → blue (spinning)
- `responding` → teal
- `error` → red

### Debug Panel (end drawer)

```
┌──────────────────────────────┐
│  Debug Panel          [Clear]│
├──────────────────────────────┤
│  State: Listening            │
├──────────────────────────────┤
│  Transitions                 │
│  14:32:01.123  Idle→Listen   │
│  14:32:03.456  Listen→Think  │
│  14:32:03.891  Think→Route   │
│  14:32:03.912  Route→Respond │
├──────────────────────────────┤
│  Tool Calls                  │
│  (none)                      │
├──────────────────────────────┤
│  Tokens                      │
│  prompt: —   completion: —   │
└──────────────────────────────┘
```

### Settings Screen

Full-screen route pushed from AppBar settings icon. Fields:
- LLM Base URL
- LLM API Key (obscured)
- LLM Model
- ElevenLabs API Key (obscured)
- Baidu API Key (obscured)
- Baidu Secret Key (obscured)
- System Instruction (multiline)
- Max Turns (numeric)

Save button validates non-empty required fields, persists to
`shared_preferences`, then restarts the bridge.

---

## CMake Integration

`ui/bridge/CMakeLists.txt` defines `shizuru_bridge` as a `SHARED` library:

```cmake
add_library(shizuru_bridge SHARED shizuru_bridge.cpp)
target_link_libraries(shizuru_bridge PRIVATE
  shizuru_runtime
  shizuru_llm_openai
  shizuru_asr_baidu_device
  shizuru_tts_elevenlabs_device
  shizuru_audio
  shizuru_vad
  shizuru_io_probe
  shizuru_baidu_utils
)
```

The root `CMakeLists.txt` adds `add_subdirectory(ui/bridge)`.

The Flutter macOS runner copies `libshizuru.dylib` into the app bundle's
`Frameworks/` directory via a build phase script or CMake `install` rule.

---

## Callback Threading Model

All C++ callbacks (`OnOutput`, state transitions) fire on internal C++ threads
(LLM streaming thread, controller loop thread). The C API marshals these onto a
dedicated `std::thread` event queue inside `ShizuruContext`. Dart receives them
via `NativeCallable.listener` which posts to the Flutter main isolate — safe for
`setState` calls.

Audio level callbacks (`shizuru_set_audio_level_callback`) are fired from the
`EnergyVadDevice` worker thread at ~20ms intervals. They are lightweight float
values and do not need the event queue — they are posted directly via a
`NativeCallable` with `keepIsolateAlive: false`.

---

## Out of Scope (deferred to later phases)

- Token usage display (requires C API extension for LLM metadata)
- Route table visualization (requires C API extension)
- Mobile targets (Android / iOS) — blocked on Phase 7 audio backends
