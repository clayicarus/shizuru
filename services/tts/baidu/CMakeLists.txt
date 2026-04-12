add_library(shizuru_tts_baidu STATIC
  baidu_tts_client.cpp
)

target_include_directories(shizuru_tts_baidu
  PUBLIC ${CMAKE_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_tts_baidu
  PUBLIC  shizuru_baidu_utils
  PRIVATE shizuru_utils
  PRIVATE CURL::libcurl
)
