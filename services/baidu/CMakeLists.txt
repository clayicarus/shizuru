add_library(shizuru_baidu STATIC
  token_manager.cpp
  tts/baidu_tts_client.cpp
  asr/baidu_asr_client.cpp
)

target_include_directories(shizuru_baidu
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(shizuru_baidu
  PUBLIC  shizuru_core
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE httplib::httplib
)
