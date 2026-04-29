# Roadmap

## Phase 1–5.5 — Infrastructure (Done)

All infrastructure phases are complete:
- Agent framework: Controller state machine, ContextStrategy, PolicyLayer, strategies
- Runtime IO: IoDevice bus, RouteTable, CoreDevice, ToolDispatchDevice
- Voice pipeline: VAD, ASR (Baidu), TTS (ElevenLabs), audio capture/playout
- Thread safety: shared_mutex, atomic flags, deadlock-free shutdown
- Controller strategies: ObservationFilter, TtsSegmentStrategy, ResponseFilter, ObservationAggregator
- Services: OpenAI LLM (libcurl + SSE), Baidu ASR/TTS, ElevenLabs TTS
- Android: Oboe audio backend with hardware 3A (VoiceCommunication preset)

## Phase 6 — Architecture Decoupling (Done)

- [x] AgentRuntime refactored to pure device bus (no business logic)
- [x] ToolDispatchDevice + ToolRegistry moved to runtime/ (DMA controller model)
- [x] Session assembly moved to app/ layer (AppRuntime)
- [x] Bridge rewritten to use AppRuntime (~600 lines removed)
- [x] Architecture documented in `.kiro/steering/architecture.md`

## Phase 7 — App Layer (Done)

- [x] `app/assembly/AppRuntime`: product-level runtime assembly
- [x] `app/persona/`: Shizuru personality prompt + dynamic context injection
- [x] `app/scheduler/SchedulerDevice`: timer-based IoDevice for reminders
- [x] `app/tools/`: builtin tools (get_current_time, get_system_info, calculate, set_reminder, save_note)
- [x] `app/memory/`: data structures for followups, preferences, notes, reminders (SqliteMemoryStore stub)
- [x] CoreDevice: `scheduler_in` port for system events (bypasses filter)
- [x] Controller: `kSystemEvent` handling in RunLoop (skip aggregator/filter)
- [x] ObservationFilter: relaxed prompt to preserve emotional expressions

## Phase 8 — Flutter UI (Done)

- [x] C API bridge (`ui/bridge/shizuru_bridge.h/.cpp`)
- [x] Dart FFI bridge (`ui/lib/bridge/shizuru_ffi.dart`)
- [x] Conversation screen with streaming message bubbles
- [x] `<think>`, `<tool_call>`, `<tool_result>` structured rendering
- [x] Settings screen with SharedPreferences persistence
- [x] Debug panel with activity log
- [x] State indicator, waveform bar, mic/speaker toggles
- [x] Message long-press copy (strips structured tags)
- [x] Android build integration (NDK + Oboe)

## Phase 9 — MVP Product Features (In Progress)

Priority for the current stage:
- Finish the user-visible MVP first.
- Do not start broad architecture cleanup before the MVP is usable end-to-end.
- Severe engineering issues found during review are tracked below in a dedicated post-MVP hardening phase, unless one of them blocks basic demo stability.
- **Current top priority:** introduce real persistent storage (SQLite-backed MemoryStore) and start storing durable session/user data before adding more dialogue behavior.

### Done
- [x] Shizuru persona (conversational Chinese style)
- [x] Reminder tool with real scheduler execution
- [x] save_note tool (session-scoped)
- [x] User custom instruction (appended to persona prompt)
- [x] Audio device shutdown ordering fix (prevent crash on exit)
- [x] Voice pathway route toggle fix (TTS route disabled before creation)
- [x] Persona prompt translated to English
- [x] Internal event format: system events (reminders, followups) wrapped in `<event>` tags, sent as `role: "user"` for cross-model compatibility

### Next
- [x] **Context window source identification**: use OpenAI API `name` field to distinguish input sources (user, voice, scheduler) instead of in-content XML tags
- [ ] **Persistent MemoryStore (SQLite)**: replace InMemoryStore so conversations survive restarts
  Current focus:
  - Introduce the real SQLite backend
  - Wire AppRuntime to use persistent storage instead of in-memory fallback
  - Persist enough session/user data to survive restart before extending memory semantics
- [ ] **Context-aware ObservationFilter**: filter should consider observation source (voice vs text vs system) and conversation state, not just content
- [ ] **Followup tools**: save_followup, list_followups, complete_followup
- [ ] **User identity**: user_id in AppConfig, memory keyed by user
- [ ] **Cross-device sync**: backend service + sync protocol

### Deferred Behavioral TODO
- [ ] **Interaction meter system**: add session-scoped signal meters (for example reply threshold, text input rate, speech rate, interruption pressure, emotional pressure) that can feed deterministic policy and future LLM hints. Do this after persistence is in place.

## Phase 10 — Post-MVP Stabilization and Architecture Hardening

Goal:
- Keep the current layered architecture (`app/`, `runtime/`, `core/`, `io/`, `services/`, `ui/`).
- After MVP completion, make the runtime and protocol layers production-safe before adding significant complexity.

### Critical correctness and stability
- [ ] **Fix AgentRuntime re-entrancy and lock model**: do not hold `devices_mutex_` while calling device `OnInput()`. Route lookup and device dispatch must be separated to avoid deadlock and re-entrant shared mutex hazards.
- [ ] **Fix SchedulerDevice concurrency bug**: remove raw pointer lifetime hazards in `TimerLoop()` and make scheduling/cancelation safe under concurrent mutation.
- [ ] **Make shutdown and callback lifecycles explicit**: unify ownership across bridge, runtime, and devices so audio callbacks, state callbacks, and runtime shutdown cannot race.

### Protocol and tool-call correctness
- [ ] **Replace hand-built tool protocol JSON**: stop building tool result payloads with string concatenation; use structured serialization/deserialization for tool calls, tool results, and activity payloads.
- [ ] **Fix multi-tool-call memory representation**: assistant tool-call history must serialize correctly for OpenAI-compatible APIs and preserve correct pairing for all tool calls, not only the first one.
- [ ] **Remove brittle ad-hoc argument parsing in builtin tools**: replace `find()` / `sscanf()` parsing with real JSON decoding.

### Session semantics and product-model correctness
- [ ] **Implement real approval flow**: `PolicyOutcome::kRequireApproval` must suspend execution and resume correctly after approval/denial instead of being treated as immediate denial.
- [ ] **Separate internal events from user messages**: add a real internal/system event memory type so reminders and followups are not persisted as `role: "user"` messages.
- [ ] **Replace placeholder summarization**: `MaybeSummarize()` must call an LLM or another real summarizer instead of writing `"Summary of N entries"`.

### Runtime and UI simplification
- [ ] **Reduce callback locking by freezing wiring before start**: once runtime wiring is complete, device output callbacks and app callbacks should be immutable where possible.
- [ ] **Remove bridge state polling thread**: push state transitions directly from the controller/bridge instead of polling every 50 ms.
- [ ] **Unify UI audio state with native device state**: mic/speaker toggles, route enablement, and device start/stop must be driven by one coherent state model.
- [ ] **Re-enable turn-trigger filtering with a robust classifier path**: semantic reply gating is currently bypassed. Restore it only with explicit cancellation, shutdown behavior, and clear separation from data-plane input sanitization.

### Architecture cleanup after hardening
- [ ] **Move AppRuntime away from hard-coded device IDs**: route assembly should depend on declared topology or named roles, not concrete implementation IDs.
- [ ] **Route table static declaration**: decouple route configuration from device lifecycle, declare routes once at create time, and remove the unused `requires_control_plane` field from `RouteOptions`.

### Deferred
- [ ] macOS CoreAudio backend (hardware 3A)
- [ ] iOS CoreAudio backend
- [ ] Windows WASAPI backend
- [ ] Software 3A (AEC/ANS/AGC) for Linux/Windows
- [ ] Device Group abstraction
- [ ] VAD upgrade (Silero)
- [ ] CI (GitHub Actions)
- [ ] Markdown stripping in ResponseFilter
- [ ] Structured rendering tag type system
- [ ] Runtime thinking mode toggle
