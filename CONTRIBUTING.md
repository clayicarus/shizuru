# Contributing to Shizuru

## Development Philosophy

Solve problems directly — no workarounds, no deferred hacks. If something is broken, fix the root cause. This project is in active early development; the architecture is intentional and should be respected when adding new code.

## Getting Started

### Prerequisites

- CMake 3.20+
- C++17-capable compiler (clang or gcc)
- PortAudio (for audio devices on desktop)
- A valid `OPENAI_API_KEY` (and optionally `BAIDU_API_KEY`, `ELEVENLABS_API_KEY`) for running examples

### Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

### Run Tests

```sh
cd build && ctest --output-on-failure
```

### Run an Example

```sh
source _env.sh   # set API keys
./build/examples/voice_agent
```

---

## Architecture Overview

The runtime is a device bus. Every component — audio capture, VAD, ASR, TTS, the agent core — is an `IoDevice`. Data flows between devices as `DataFrame` packets routed by a `RouteTable`. `AgentRuntime` owns the bus and does zero data transformation.

```
IoDevice  →  OutputCallback  →  AgentRuntime::DispatchFrame  →  RouteTable  →  IoDevice::OnInput
```

Key invariants to preserve:

- `AgentRuntime::DispatchFrame` must never transform frame data.
- `CoreDevice` is the only place that translates between `DataFrame` and core types (`Observation`, `ActionCandidate`).
- All control decisions originate from `CoreDevice` (i.e., from the LLM or user input). IO devices are passive — they sense and execute, they do not decide.
- `RouteTable` is the single source of truth for all data flow topology. Do not hardcode device-to-device calls.
- DMA routes (`requires_control_plane = false`) must not involve the LLM or controller in their data path.

See `AGENTS.md` for the full architecture reference.

---

## Adding a New Vendor Implementation

Follow the existing layout:

```
services/<module>/<vendor>/   ← vendor HTTP client only
io/<module>/<vendor>/         ← IoDevice wrapper
```

Steps:
1. Create `services/<module>/<vendor>/` with a `CMakeLists.txt` defining a library target.
2. Create `io/<module>/<vendor>/` with a `CMakeLists.txt` defining a separate library target.
3. Add `add_subdirectory(<vendor>)` in the parent `CMakeLists.txt` of each.
4. If the vendor needs shared auth (e.g., token refresh), add it to `services/utils/<vendor>/`.
5. Do not add vendor source files directly to a parent CMakeLists target.

---

## Planned Work

The following tracks are prioritized in order. Please coordinate before starting work on Phase B or C to avoid conflicts.

### Phase A — Thread Safety (Done)

T1-1 through T1-6 are complete. T1-7 is planned.

- **T1-1** ✅ `AgentRuntime::DispatchFrame`: `std::shared_mutex` added to protect `devices_` and `route_table_`. `DispatchFrame` holds a shared lock; `RegisterDevice`, `UnregisterDevice`, `AddRoute`, `RemoveRoute`, and `Shutdown` hold a unique lock.
- **T1-2** ✅ `BaiduAsrDevice::Flush()`: blocking `join` removed from PortAudio callback thread. Internal worker thread + task queue introduced; `Flush()` snapshots audio and posts a task, returning immediately.
- **T1-3** ✅ `ElevenLabsTtsDevice::OnInput`: blocking `join` removed from `Controller::loop_thread_`. Same pattern: internal worker thread + task queue; `OnInput` posts text and returns immediately.
- **T1-4** ✅ `CoreDevice::active_`: changed from `bool` to `std::atomic<bool>`.
- **T1-5** ✅ `Controller` callbacks: `OnResponse`, `OnTransition`, `OnDiagnostic` now assert that `loop_thread_` is not yet running, enforcing pre-`Start()` registration.
- **T1-6** ✅ `AudioPlayoutDevice`: debug `static fopen` / `fwrite` removed from production code path.
- **T1-7** `IoExecutor`: introduce a shared thread pool in `AgentRuntime` for network I/O tasks. `BaiduAsrDevice` and `ElevenLabsTtsDevice` replace their per-device worker threads with an injected `Executor&`. Audio-path devices (capture, VAD, playout) are unaffected — they run on PortAudio's real-time thread and must not share the pool. Enables future migration to a lock-free MPSC queue without changing device code.

### Phase B — Core / Tool Call Decoupling (Priority: Medium)

Decouple `Controller` from `IoBridge` so that tool execution is a proper IO round-trip through the device bus, not a synchronous in-process call.

- **T2-1** Remove `std::unique_ptr<IoBridge> io_` from `Controller`. `HandleActing` emits an `action/tool_call` DataFrame on `action_out`, leaves state in `kActing`, and waits for a `kToolResult` observation to arrive via the queue. Inject a `CancelCallback` (`std::function<void()>`) for interrupt handling.
- **T2-2** Remove `InterceptingIoBridge` from `CoreDevice`. The `action_out` emit logic moves into `Controller` directly.
- **T2-3** Add `ToolDispatchDevice` (`services/io/tool_dispatch_device.h/.cpp`): an `IoDevice` that holds a `ToolRegistry&`, receives `action/tool_call` frames on `action_in`, executes the tool, and emits `action/tool_result` frames on `result_out`.
- **T2-4** In `AgentRuntime::StartSession`, register `ToolDispatchDevice` and add routes:
  ```
  core:action_out          → tool_dispatch:action_in
  tool_dispatch:result_out → core:tool_result_in
  ```
- **T2-5** Update `core_device_test` and `controller_test` to remove `IoBridge` mocks and test the new async round-trip.

### Phase C — Control Plane via DataFrame (Priority: Medium)

All control signals originate from `CoreDevice`. IO devices are passive and receive commands through a dedicated `control_in` port, keeping `RouteTable` as the single topology source of truth.

- **T3-1** Add `control_out` output port to `CoreDevice`. `Controller` emits control frames (e.g., on `HandleInterrupt`, `HandleResponding`) using a simple JSON payload: `{"command": "cancel"}`, `{"command": "flush"}`.
- **T3-2** Define the control frame protocol in a shared header (`io/control_frame.h` or similar): enumerate supported commands and their payloads.
- **T3-3** Add `control_in` input port to `ElevenLabsTtsDevice`, `AudioPlayoutDevice`, and `BaiduAsrDevice`. Each device responds to the commands it supports (`cancel`, `flush`, etc.).
- **T3-4** Remove the direct `asr_ptr->Flush()` call from `VadEventDevice`. Replace with: VAD emits `vad/event` → CoreDevice receives it as an observation → CoreDevice emits `control/flush` on `control_out` → routed to `BaiduAsrDevice:control_in`. All control flow now passes through core.
- **T3-5** Update `voice_agent.cpp` and other examples to add the new control routes.

### Phase D — Software 3A for Desktop (Priority: Low, desktop-only)

Mobile platforms (Android/iOS) expose hardware AEC, ANS, and AGC at the OS level and operate at 16 kHz — sufficient for both ASR input and TTS playout. No software processing is needed there.

Desktop platforms (macOS/Linux/Windows via PortAudio) have no hardware 3A. The following `IoDevice` implementations are required for production-quality voice on desktop. Each is inserted into the capture chain between `AudioCaptureDevice` and `EnergyVadDevice`.

Recommended library: [WebRTC Audio Processing Module](https://chromium.googlesource.com/external/webrtc/) (APM) — provides AEC3, NS, and AGC2 in a single C++ library, well-tested at 16 kHz.

- **T4-1** `AecDevice` (`io/audio/aec/`): Acoustic Echo Cancellation. Receives the capture signal on `audio_in` and the playout reference signal on `reference_in`; emits the echo-cancelled signal on `audio_out`. Prevents the ASR from transcribing the agent's own TTS output.
  - Route: `audio_capture:audio_out → aec:audio_in`, `audio_playout:monitor_out → aec:reference_in`, `aec:audio_out → vad:audio_in`
- **T4-2** `AnsDevice` (`io/audio/ans/`): Ambient Noise Suppression. Single-port filter (`audio_in` → `audio_out`). Reduces stationary background noise before VAD/ASR.
- **T4-3** `AgcDevice` (`io/audio/agc/`): Automatic Gain Control. Single-port filter (`audio_in` → `audio_out`). Normalizes capture amplitude to keep ASR input within a consistent level range.
- **T4-4** CMake gating: wrap `io/audio/aec`, `io/audio/ans`, `io/audio/agc` subdirectories in `if(NOT ANDROID AND NOT IOS)` so mobile builds skip them entirely.
- **T4-5** `AudioPlayoutDevice`: add a `monitor_out` output port that emits a copy of each written frame (needed as the AEC reference signal). This port is only connected when AEC is in use.

Capture chain with software 3A (desktop):
```
AudioCaptureDevice → AecDevice → AnsDevice → AgcDevice → EnergyVadDevice → BaiduAsrDevice
                         ↑
              AudioPlayoutDevice:monitor_out
```

Capture chain without software 3A (mobile — hardware handles it):
```
AudioCaptureDevice → EnergyVadDevice → BaiduAsrDevice
```

---

## Code Style

- C++17. No exceptions in hot paths (audio callbacks). RAII everywhere.
- Follow the existing file and namespace layout (`shizuru::io`, `shizuru::core`, `shizuru::runtime`, `shizuru::services`).
- New `IoDevice` implementations must implement all six interface methods: `GetDeviceId`, `GetPortDescriptors`, `OnInput`, `SetOutputCallback`, `Start`, `Stop`.
- Tests live in `tests/<module>/`. Use GoogleTest. Property-based tests use RapidCheck (see existing examples in `tests/agent/`).
- Do not add vendor source files directly to a parent CMakeLists target — each vendor gets its own CMakeLists and library target.
