# Shizuru

A cross-platform voice conversation agent built in C++17, with a Flutter UI and OpenAI-compatible LLM backend.

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
./build/examples/tool_call_example

# Local Ollama
./build/examples/tool_call_example http://localhost:11434 "" qwen3:8b /api/chat

# OpenAI
./build/examples/tool_call_example https://api.openai.com sk-your-key gpt-4o
```

**Voice echo pipeline** — microphone → VAD → Baidu ASR → Baidu TTS → speaker:
```bash
export BAIDU_API_KEY=...
export BAIDU_SECRET_KEY=...
./build/examples/asr_tts_echo_pipeline
```

**Full voice agent** — microphone → VAD → ASR → LLM → TTS → speaker:
```bash
export BAIDU_API_KEY=...
export BAIDU_SECRET_KEY=...
export OPENAI_API_KEY=...
./build/examples/voice_agent [--base-url <url>] [--model <model>] [--debug]
```

## Project structure

```
core/        Agent framework: controller, context strategy, policy, session
services/    Vendor clients: LLM (OpenAI), ASR (Baidu), TTS (Baidu, ElevenLabs)
io/          IoDevice abstraction, audio capture/playout, VAD, ASR/TTS device wrappers
runtime/     AgentRuntime (device bus), CoreDevice, RouteTable
examples/    Runnable examples
tests/       Unit and property-based tests
```

## Architecture

The runtime is a device bus. Every component — including the agent session — is an `IoDevice`. Data flows as typed `DataFrame` packets routed by a `RouteTable`.

```
Microphone
    │  audio/pcm (DMA)
    ▼
EnergyVadDevice  ──vad/event──►  VadEventDevice (triggers ASR flush)
    │  audio/pcm (speech frames only, with pre-roll)
    ▼
BaiduAsrDevice
    │  text/plain
    ▼
CoreDevice (AgentSession — LLM reasoning loop)
    │  text/plain  (via OnOutput callback)
    ▼
BaiduTtsDevice
    │  audio/pcm (DMA)
    ▼
Speaker
```

DMA routes (`requires_control_plane = false`) bypass the LLM loop for low-latency audio. The agent core is modeled after an OS: controller as state machine, LLM as CPU, context strategy as memory manager, policy layer as permission boundary.

## Cross-platform

| Platform      | Audio backend      | UI      |
|---------------|--------------------|---------|
| macOS / Linux | PortAudio          | Flutter |
| Windows       | PortAudio / WASAPI | Flutter |
| Android       | Oboe               | Flutter |
| iOS           | CoreAudio          | Flutter |

C++ core is shared across all platforms. Platform-specific code lives behind abstract interfaces.
