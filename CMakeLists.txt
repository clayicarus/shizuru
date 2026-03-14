cmake_minimum_required(VERSION 3.20)
project(shizuru LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# ── spdlog (async, high-performance logger) ──────────────────────────
include(FetchContent)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.1
)
FetchContent_MakeAvailable(spdlog)

add_subdirectory(core)
add_subdirectory(io/audio)
add_subdirectory(services)
add_subdirectory(runtime)
add_subdirectory(tests/agent)
add_subdirectory(tests/services)
