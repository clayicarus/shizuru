include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  httplib
  URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(httplib)

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
  PRIVATE httplib::httplib
)
