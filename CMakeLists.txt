cmake_minimum_required(VERSION 3.20)
project(shizuru LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(utils)
add_subdirectory(core)
add_subdirectory(io/audio)
add_subdirectory(services)
add_subdirectory(runtime)
add_subdirectory(examples)
add_subdirectory(tests/agent)
add_subdirectory(tests/services)
