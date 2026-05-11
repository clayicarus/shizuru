include(FetchContent)

# Google Test
FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# RapidCheck
set(RC_ENABLE_GTEST ON CACHE BOOL "Build RapidCheck Google Test integration" FORCE)
FetchContent_Declare(
  rapidcheck
  URL      https://github.com/emil-e/rapidcheck/archive/refs/heads/master.tar.gz
)
FetchContent_MakeAvailable(rapidcheck)

# ControlFrame tests removed — ControlFrame type was deleted in unified pipeline refactoring.

# VadEventDevice and IO device control_in tests removed — 
# referenced deleted ControlFrame/InterruptFrame types.

add_executable(vad_event_device_typed_test
  vad_event_device_typed_test.cpp
)
target_link_libraries(vad_event_device_typed_test
  PRIVATE shizuru_vad
  PRIVATE GTest::gtest_main
)
target_include_directories(vad_event_device_typed_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME vad_event_device_typed_test COMMAND vad_event_device_typed_test)
