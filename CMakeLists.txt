cmake_minimum_required(VERSION 3.20)

# CMake 4.x removed compatibility with policies older than 3.5.
# FetchContent dependencies (PortAudio, curl, spdlog) may use older
# cmake_minimum_required values.  This variable restores compatibility.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
      "Minimum policy version for FetchContent compatibility")
endif()
project(shizuru LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake 3.24+ warns about DOWNLOAD_EXTRACT_TIMESTAMP in FetchContent.
# Set the default to TRUE globally so individual FetchContent_Declare calls
# don't need the flag (which breaks CMake < 3.24 like Android SDK's 3.22).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

enable_testing()

add_subdirectory(utils)
add_subdirectory(core)
add_subdirectory(io/audio)
add_subdirectory(io/asr)
add_subdirectory(io/tts)
add_subdirectory(io/probe)
add_subdirectory(io/vad)
add_subdirectory(services)
add_subdirectory(runtime)
add_subdirectory(ui/bridge)

if(NOT ANDROID)
  # Desktop-only: examples and tests (require PortAudio hardware / host tools)
  add_subdirectory(examples)
  add_subdirectory(tests/agent)
  add_subdirectory(tests/io)
  add_subdirectory(tests/services)
  add_subdirectory(tests/runtime)
  add_subdirectory(tests/vad)
endif()
