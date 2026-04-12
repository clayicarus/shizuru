include(FetchContent)

FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(vad_test vad_test.cpp)

target_link_libraries(vad_test
  PRIVATE shizuru_vad
  PRIVATE GTest::gtest_main
)

target_include_directories(vad_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)

add_test(NAME vad_test COMMAND vad_test)
