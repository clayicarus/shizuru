# Shizuru

A cross-platform voice conversation agent built in C++17, with a Flutter UI and OpenAI-compatible LLM backend.

## Requirements

- CMake 3.20+
- C++17 compiler (clang or gcc)
- Ninja (recommended)
- OpenSSL (for HTTPS — already present on most systems)
- PortAudio (for audio on desktop)

On macOS:
```bash
brew install cmake ninja openssl portaudio
```

## Build

```bash
cmake -B build -G Ninja
cmake --build build
```

## Run the example

```bash
# No LLM needed — uses a built-in mock server
./build/examples/tool_call_example

# Connect to a local Ollama instance
./build/examples/tool_call_example http://localhost:11434 "" qwen3:8b /api/chat

# Connect to OpenAI
./build/examples/tool_call_example https://api.openai.com sk-your-key gpt-4o
```

The example runs an interactive agent loop with a `get_weather` tool. Type a message, press Enter. Type `quit` or Ctrl+D to exit.

## Run tests

```bash
ctest --test-dir build
```

## Project structure

```
core/        Agent framework: controller, context strategy, policy, session
services/    Concrete implementations: LLM client, tool dispatcher, memory, audit
io/audio/    Audio subsystem: abstract interfaces + PortAudio backend (desktop)
runtime/     AgentRuntime: assembles all components, manages session lifecycle
examples/    Runnable examples
tests/       Unit and property-based tests
```

## Architecture overview

The agent is modeled after an OS:

- Controller — state machine, drives the reasoning loop
- LLM client — OpenAI-compatible HTTP + SSE streaming
- Context strategy — builds the prompt window from memory
- Policy layer — permission checks and audit before every tool call
- IO bridge — tool registry and dispatcher

Voice capabilities (VAD, ASR, TTS) are registered as tools, not hard-wired into the pipeline. The controller decides when to invoke them.

## Cross-platform

| Platform      | Audio backend      | UI      |
|---------------|--------------------|---------|
| macOS / Linux | PortAudio          | Flutter |
| Windows       | PortAudio / WASAPI | Flutter |
| Android       | Oboe               | Flutter |
| iOS           | CoreAudio          | Flutter |

The C++ core is shared across all platforms. Platform-specific code lives behind abstract interfaces.
