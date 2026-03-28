add_library(shizuru_bridge SHARED shizuru_bridge.cpp)

# Set install name for macOS so the dylib is found relative to the app bundle.
if(APPLE)
  set_target_properties(shizuru_bridge PROPERTIES
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_RPATH TRUE
  )
endif()

target_link_libraries(shizuru_bridge PRIVATE
  shizuru_runtime
  shizuru_llm_openai
  shizuru_asr_baidu_device
  shizuru_tts_elevenlabs_device
  shizuru_audio
  shizuru_vad
  shizuru_io_probe
  shizuru_baidu_utils
  nlohmann_json::nlohmann_json
)

target_include_directories(shizuru_bridge PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/core
  ${CMAKE_SOURCE_DIR}/io
  ${CMAKE_SOURCE_DIR}/io/audio
  ${CMAKE_SOURCE_DIR}/io/audio/audio_device
  ${CMAKE_SOURCE_DIR}/io/asr
  ${CMAKE_SOURCE_DIR}/io/tts
  ${CMAKE_SOURCE_DIR}/io/vad
  ${CMAKE_SOURCE_DIR}/io/probe
  ${CMAKE_SOURCE_DIR}/runtime
  ${CMAKE_SOURCE_DIR}/services
  ${CMAKE_SOURCE_DIR}/services/llm
  ${CMAKE_SOURCE_DIR}/services/utils
  ${CMAKE_SOURCE_DIR}/services/tts
  ${CMAKE_SOURCE_DIR}/services/io
  ${CMAKE_SOURCE_DIR}/utils
)

# Ensure the public header is visible to consumers (e.g. test programs).
target_include_directories(shizuru_bridge PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
