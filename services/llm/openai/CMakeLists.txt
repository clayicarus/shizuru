add_library(shizuru_llm_openai STATIC
  openai_client.cpp
  json_parser.cpp
)

target_include_directories(shizuru_llm_openai
  PUBLIC ${CMAKE_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_llm_openai
  PUBLIC  shizuru_core
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE CURL::libcurl
)
