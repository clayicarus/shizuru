add_library(shizuru_asr_baidu_device STATIC
  baidu_asr_device.cpp
)

target_include_directories(shizuru_asr_baidu_device
  PUBLIC ${CMAKE_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_asr_baidu_device
  PUBLIC  shizuru_asr_baidu
  PRIVATE shizuru_utils
)
