include(FetchContent)

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
