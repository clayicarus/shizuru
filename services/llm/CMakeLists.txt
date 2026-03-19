include(FetchContent)

# nlohmann/json (header-only JSON library) — URL download is much faster than git clone
FetchContent_Declare(
  nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# cpp-httplib (header-only HTTP library) — URL download
FetchContent_Declare(
  httplib
  URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# OpenSSL detection is handled in the root CMakeLists.txt.
# HTTPLIB_REQUIRE_OPENSSL is already set; just make httplib available.
FetchContent_MakeAvailable(httplib)

add_library(shizuru_llm STATIC
  json_parser.cpp
  openai_client.cpp
)

target_include_directories(shizuru_llm
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(shizuru_llm
  PUBLIC  shizuru_core
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE httplib::httplib
)
