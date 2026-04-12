include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL      https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(shizuru_tts_elevenlabs STATIC
  elevenlabs_client.cpp
)

target_include_directories(shizuru_tts_elevenlabs
  PUBLIC ${CMAKE_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}/services
)

target_link_libraries(shizuru_tts_elevenlabs
  PUBLIC  shizuru_utils
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE CURL::libcurl
)
