include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Persona tests
add_executable(persona_test persona_test.cpp)
target_link_libraries(persona_test PRIVATE shizuru_app GTest::gtest_main)
target_include_directories(persona_test PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME persona_test COMMAND persona_test)

add_executable(app_runtime_replay_test app_runtime_replay_test.cpp)
target_link_libraries(app_runtime_replay_test PRIVATE shizuru_app GTest::gtest_main)
target_include_directories(app_runtime_replay_test PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME app_runtime_replay_test COMMAND app_runtime_replay_test)
