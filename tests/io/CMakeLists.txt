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

# ControlFrame tests (unit + property)
add_executable(control_frame_test
  control_frame_test.cpp
)
target_link_libraries(control_frame_test
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(control_frame_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME control_frame_test COMMAND control_frame_test)
set_tests_properties(control_frame_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# VadEventDevice tests (unit + property)
add_executable(vad_event_device_test
  vad_event_device_test.cpp
)
target_link_libraries(vad_event_device_test
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
  PRIVATE shizuru_vad
)
target_include_directories(vad_event_device_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME vad_event_device_test COMMAND vad_event_device_test)
set_tests_properties(vad_event_device_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=100")

# IO device control_in tests (unit + property) — Property 9
add_executable(io_device_control_in_test
  io_device_control_in_test.cpp
)
target_link_libraries(io_device_control_in_test
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
  PRIVATE shizuru_tts_elevenlabs_device
  PRIVATE shizuru_asr_baidu_device
  PRIVATE shizuru_audio
)
target_include_directories(io_device_control_in_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
)
add_test(NAME io_device_control_in_test COMMAND io_device_control_in_test)
set_tests_properties(io_device_control_in_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=100")
