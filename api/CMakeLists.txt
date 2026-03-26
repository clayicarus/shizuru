# Shared library exposing the C API for dart:ffi and other host embeddings.

add_library(shizuru_api SHARED
  shizuru_api.cpp
)

target_compile_definitions(shizuru_api PRIVATE SHIZURU_API_EXPORTS)

target_include_directories(shizuru_api
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/io
  PRIVATE ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_api
  PRIVATE shizuru_runtime
  PRIVATE shizuru_audio
  PRIVATE shizuru_tts_baidu_device
  PRIVATE shizuru_tts_elevenlabs_device
  PRIVATE shizuru_asr_baidu
  PRIVATE shizuru_baidu_utils
)
