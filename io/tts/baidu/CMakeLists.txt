add_library(shizuru_tts_baidu_device STATIC
  baidu_tts_device.cpp
)

target_include_directories(shizuru_tts_baidu_device
  PUBLIC ${CMAKE_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_tts_baidu_device
  PUBLIC  shizuru_tts_baidu
  PRIVATE shizuru_utils
)
