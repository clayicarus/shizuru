add_library(shizuru_baidu_utils STATIC
  baidu_token_manager.cpp
)

target_include_directories(shizuru_baidu_utils
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(shizuru_baidu_utils
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE shizuru_utils
  PRIVATE CURL::libcurl
)
