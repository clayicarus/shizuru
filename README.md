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
export ELEVENLABS_API_KEY=...
./build/examples/voice_agent [--base-url <url>] [--model <model>] [--voice-id <id>] [--debug]
```

PCM dumps are written to the working directory: `capture.pcm`, `vad_dump.pcm`, `playout_dump.pcm` (raw s16le 16 kHz mono).

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
PcmDumpDevice (capture.pcm)
    │  audio/pcm (DMA)
    ▼
EnergyVadDevice ──vad/event──► VadEventDevice ──vad/event──► CoreDevice:vad_in
    │  audio/pcm (speech frames only, with pre-roll)                │
    ▼                                                               │ control/command (flush → ASR, cancel → TTS/playout)
PcmDumpDevice (vad_dump.pcm)                                        ▼
    │  audio/pcm (DMA)                              ┌─────────────────────────────┐
    ▼                                               │  CoreDevice (AgentSession)  │
BaiduAsrDevice ──text/plain──────────────────────► │  LLM reasoning loop         │
                                                    │  action_out ──► ToolDispatch│
                                                    └──────┬──────────────────────┘
                                                           │  text/plain
                                                           ▼
                                                   ElevenLabsTtsDevice
                                                           │  audio/pcm (DMA)
                                                           ▼
                                                   PcmDumpDevice (playout_dump.pcm)
                                                           │  audio/pcm (DMA)
                                                           ▼
                                                        Speaker
```

DMA routes (`requires_control_plane = false`) bypass the LLM loop for low-latency audio. Control commands (`cancel`, `flush`) flow from `CoreDevice:control_out` to `BaiduAsrDevice`, `ElevenLabsTtsDevice`, and `AudioPlayoutDevice` via the control plane. On VAD `speech_start`, TTS and playout are cancelled immediately; on `speech_end`, ASR is flushed.

The agent core is modeled after an OS: controller as state machine, LLM as CPU, context strategy as memory manager, policy layer as permission boundary.

## Cross-platform

| Platform      | Audio backend      | UI      |
|---------------|--------------------|---------|
| macOS / Linux | PortAudio          | Flutter |
| Windows       | PortAudio / WASAPI | Flutter |
| Android       | Oboe               | Flutter |
| iOS           | CoreAudio          | Flutter |

C++ core is shared across all platforms. Platform-specific code lives behind abstract interfaces.
