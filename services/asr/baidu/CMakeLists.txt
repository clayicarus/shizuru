add_library(shizuru_asr_baidu STATIC
  baidu_asr_client.cpp
)

target_include_directories(shizuru_asr_baidu
  PUBLIC ${CMAKE_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_asr_baidu
  PUBLIC  shizuru_baidu_utils
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE shizuru_utils
  PRIVATE CURL::libcurl
)
