include(FetchContent)

# httplib is used only for the mock HTTP server in tool_call example.
FetchContent_Declare(
  httplib
  URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# Audio loopback example (requires PortAudio hardware)
add_executable(audio_loopback audio_loopback.cpp)
target_link_libraries(audio_loopback PRIVATE shizuru_audio)

# Tool call example (self-contained with mock LLM server)
add_executable(tool_call_example tool_call.cpp)
target_link_libraries(tool_call_example
  PRIVATE shizuru_runtime
  PRIVATE shizuru_llm_openai
  PRIVATE httplib::httplib
  PRIVATE nlohmann_json::nlohmann_json
)

# WAV file playout example
add_executable(wav_playout wav_playout.cpp)
target_link_libraries(wav_playout PRIVATE shizuru_audio)

# TTS playout example: ElevenLabs streaming TTS → PortAudio player
add_executable(elevenlabs_tts_playout elevenlabs_tts_playout.cpp)
target_link_libraries(elevenlabs_tts_playout
  PRIVATE shizuru_tts_elevenlabs
  PRIVATE shizuru_audio
)

# Baidu voice integration test
add_executable(baidu_voice_test baidu_voice_test.cpp)
target_link_libraries(baidu_voice_test
  PRIVATE shizuru_asr_baidu
  PRIVATE shizuru_tts_baidu
)

# Voice pipeline: Microphone → VAD → Baidu ASR → Baidu TTS → Speaker
add_executable(asr_tts_echo_pipeline asr_tts_echo_pipeline.cpp)
target_link_libraries(asr_tts_echo_pipeline
  PRIVATE shizuru_vad
  PRIVATE shizuru_asr_baidu_device
  PRIVATE shizuru_tts_baidu_device
  PRIVATE shizuru_io_probe
  PRIVATE shizuru_audio
  PRIVATE shizuru_utils
)
target_include_directories(asr_tts_echo_pipeline
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
  PRIVATE ${CMAKE_SOURCE_DIR}/io
)

# Voice agent: Microphone → VAD → Baidu ASR → LLM (AgentRuntime) → ElevenLabs TTS → Speaker
add_executable(voice_agent voice_agent.cpp)
target_link_libraries(voice_agent
  PRIVATE shizuru_runtime
  PRIVATE shizuru_vad
  PRIVATE shizuru_asr_baidu_device
  PRIVATE shizuru_tts_elevenlabs_device
  PRIVATE shizuru_audio
  PRIVATE shizuru_io_probe
  PRIVATE shizuru_llm_openai
  PRIVATE shizuru_utils
)
target_include_directories(voice_agent
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
  PRIVATE ${CMAKE_SOURCE_DIR}/io
)

# Multimodal chat: test interleaved text + image_url content parts
add_executable(multimodal_chat multimodal_chat.cpp)
target_link_libraries(multimodal_chat
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE CURL::libcurl
)

# OneBot 11 agent: Reverse WebSocket → CoreDevice → console + private reply
# SQLite3 for persistence (reuse app/'s find logic).
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND AND NOT TARGET SQLite::SQLite3)
  # Already fetched by app/ — just reference the alias.
endif()

add_executable(onebot_agent onebot_agent.cpp
  ${CMAKE_SOURCE_DIR}/app/memory/sqlite_memory_store.cpp
)
target_link_libraries(onebot_agent
  PRIVATE shizuru_core
  PRIVATE shizuru_runtime
  PRIVATE shizuru_io_onebot
  PRIVATE shizuru_llm_openai
  PRIVATE shizuru_utils
  PRIVATE SQLite::SQLite3
)
target_include_directories(onebot_agent
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/core
  PRIVATE ${CMAKE_SOURCE_DIR}/services
  PRIVATE ${CMAKE_SOURCE_DIR}/utils
)
