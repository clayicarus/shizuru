# Shizuru Flutter UI

Flutter desktop UI prototype for the Shizuru AI Agent.

## Quick Start

```bash
# 1. Install Flutter SDK (if not already)
#    https://docs.flutter.dev/get-started/install/windows/desktop

# 2. Enable Windows desktop support
flutter config --enable-windows-desktop

# 3. Install dependencies
cd ui
flutter pub get

# 4. Run (desktop)
flutter run -d windows
```

## Project Structure

```
ui/
├── lib/
│   ├── main.dart                    # Entry point
│   ├── app.dart                     # MaterialApp + theme + routes
│   ├── bridge/
│   │   └── shizuru_bridge.dart      # dart:ffi stub (mock for prototype)
│   ├── models/
│   │   ├── agent_state.dart         # AgentState enum + display helpers
│   │   ├── message.dart             # ChatMessage (user/assistant/tool)
│   │   └── session.dart             # ChatSession (message list + title)
│   ├── providers/
│   │   └── app_provider.dart        # All state (ChangeNotifier)
│   ├── pages/
│   │   ├── main_layout.dart         # 3-column desktop layout
│   │   └── settings_page.dart       # API key / model / prompt config
│   └── widgets/
│       ├── agent_state_badge.dart   # Pulsing status badge in AppBar
│       ├── session_sidebar.dart     # Left panel: session history list
│       ├── conversation_view.dart   # Center: message list + thinking dots
│       ├── chat_bubble.dart         # User / assistant / tool bubbles
│       ├── input_bar.dart           # Text input + voice mode toggle
│       └── debug_panel.dart         # Right panel: state / token / logs
└── pubspec.yaml
```

## UI Layout

```
┌───────────────────────────────────────────────────────────────────────┐
│ ≡  Shizuru   [● 就绪]                          [🔧 调试]  [⚙ 设置]   │
├──────────────┬────────────────────────────────────┬───────────────────┤
│              │                                    │  调试面板          │
│  历史侧栏    │        对话区域                     │                   │
│  (248px)     │                                    │  [● 就绪]         │
│              │   [AI] 你好！有什么可以帮你的？    │  Token 1240/4096  │
│  > 新建对话  │                                    │  ─────────────    │
│              │   [用户] 帮我写一段代码             │  工具调用│日志    │
│  新对话      │                                    │  ─────────────    │
│  帮我写一…   │   [⚙ web_search]                   │  ✓ web_search     │
│  解释一个…   │                                    │                   │
│              │   [AI] 好的，以下是示例代码…▌      │                   │
│              │                                    │                   │
│              ├────────────────────────────────────┤                   │
│              │ [🎤] [___输入消息…___________] [→] │                   │
└──────────────┴────────────────────────────────────┴───────────────────┘
```

## Wiring Real C++ (Phase 7)

The `ShizuruBridge` abstraction in `lib/bridge/shizuru_bridge.dart` is the
only file that needs to change when integrating with the C++ runtime.

Steps:
1. Expose a C API from `AgentRuntime` (e.g., `shizuru_ffi.h`):
   ```c
   extern "C" {
     const char* shizuru_start_session();
     void        shizuru_send_message(const char* text);
     int         shizuru_get_state();
     void        shizuru_on_output(ShizuruOutputCallback cb);
     void        shizuru_on_state_change(ShizuruStateCallback cb);
     void        shizuru_shutdown();
   }
   ```
2. Create `_FfiBridge extends ShizuruBridge` in the bridge file that
   calls these functions via `dart:ffi`.
3. In `main.dart`, set `ShizuruBridge._instance = _FfiBridge()` before
   `runApp()`.
4. No other files need to change.
