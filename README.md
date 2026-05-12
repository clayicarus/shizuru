# Shizuru

A cross-platform companion assistant built in C++17, with a Flutter UI and OpenAI-compatible LLM backend.

Shizuru remembers what matters to you, follows up naturally over time, and helps you take the next small step.

## Requirements

- CMake 3.20+
- C++17 compiler (clang or gcc)
- Ninja (recommended)
- OpenSSL
- PortAudio (desktop audio)

On macOS:
```bash
brew install cmake ninja openssl portaudio
```

## Build

```bash
cmake -B build -G Ninja
cmake --build build
```

## Test

```bash
ctest --test-dir build
```

## Examples

**Text agent with tool calling** (no audio hardware needed):
```bash
# Built-in mock LLM — no API key required
./build/examples/voice_agent

# Local Ollama
./build/examples/voice_agent --base-url http://localhost:11434 --model qwen3:8b

# OpenAI
./build/examples/voice_agent --base-url https://api.openai.com --model gpt-4o
```

**Full voice agent** — microphone → VAD → ASR → LLM → TTS → speaker:
```bash
export BAIDU_API_KEY=...
export BAIDU_SECRET_KEY=...
export OPENAI_API_KEY=...
export ELEVENLABS_API_KEY=...
./build/examples/voice_agent [--base-url <url>] [--model <model>] [--voice-id <id>] [--debug]
```

## Project Structure

```
app/          Product layer: persona, scheduler, tools, memory, AppRuntime
core/         Agent framework: dialogue reducer, controller state machine, context, policy, strategies
io/           IoDevice implementations: audio, ASR, TTS, VAD, probes
runtime/      Device bus: AgentRuntime, CoreDevice, ToolDispatchDevice, RouteTable
services/     Vendor clients: OpenAI LLM, Baidu ASR/TTS, ElevenLabs TTS
ui/           Flutter app + C bridge (dart:ffi)
examples/     Runnable examples
tests/        Unit, property-based, and integration tests
```

## Architecture

The runtime is a device bus. Every component — including the agent session — is an `IoDevice`. Data flows as typed payloads routed by a `RouteTable`: `AudioFrame` for audio, `ConversationItem` for semantics, and `ControlSignal` for control plane events.

```
Microphone → VAD → ASR → CoreDevice (LLM reasoning) → TTS → Speaker
                              ↕
                     ToolDispatchDevice (tool execution)
                              ↕
                     SchedulerDevice (reminders, followups)
```

The app layer (`app/`) sits above the infrastructure and handles product-specific logic: persona prompt assembly, tool registration, scheduler wiring, and session lifecycle.

Current core architecture status:

- `controller/` still owns the main execution state machine and normal user input path
- `dialogue/DefaultDialogueReducer` is already introduced for Phase 1 and owns the text/message-layer `barge-in + debounce` branch
- normal user input still follows aggregator → filter → thinking
- VAD interruption and ASR flush now travel on typed `ControlSignal` routes through the runtime bus

See `.kiro/steering/architecture.md` for the full architecture guide.

## Cross-platform

| Platform      | Audio backend | UI      |
|---------------|---------------|---------|
| macOS / Linux | PortAudio     | Flutter |
| Windows       | PortAudio     | Flutter |
| Android       | Oboe          | Flutter |
| iOS           | CoreAudio     | Flutter |

C++ core is shared across all platforms. Platform-specific code lives behind abstract interfaces.
