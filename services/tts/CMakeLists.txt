include(FetchContent)

# Reuse nlohmann/json and httplib if already fetched by shizuru_llm.
# FetchContent deduplicates by name, so these are no-ops if already declared.
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
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
else()
  set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(httplib)

add_library(shizuru_tts STATIC
  elevenlabs_client.cpp
)

target_include_directories(shizuru_tts
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(shizuru_tts
  PUBLIC  shizuru_utils
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE httplib::httplib
)
