# Audio loopback example (requires PortAudio hardware)
add_executable(audio_loopback audio_loopback.cpp)
target_link_libraries(audio_loopback PRIVATE shizuru_audio)

# Tool call example (self-contained with mock LLM server)
add_executable(tool_call_example tool_call.cpp)
target_link_libraries(tool_call_example
  PRIVATE shizuru_runtime
  PRIVATE httplib::httplib
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

# Voice pipeline: Microphone → Baidu ASR → Baidu TTS → Speaker
add_executable(voice_pipeline voice_pipeline.cpp)
target_link_libraries(voice_pipeline
  PRIVATE shizuru_asr_baidu_device
  PRIVATE shizuru_tts_baidu_device
  PRIVATE shizuru_io_probe
  PRIVATE shizuru_audio
  PRIVATE shizuru_utils
)
target_include_directories(voice_pipeline
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
  PRIVATE ${CMAKE_SOURCE_DIR}/io
)
