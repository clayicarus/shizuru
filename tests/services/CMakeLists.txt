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

# ToolRegistry tests
add_executable(tool_dispatcher_test
  tool_dispatcher_test.cpp
)
target_link_libraries(tool_dispatcher_test
  PRIVATE shizuru_runtime
  PRIVATE GTest::gtest_main
)
add_test(NAME tool_dispatcher_test COMMAND tool_dispatcher_test)

# ElevenLabs TTS device tests (unit + property)
add_executable(elevenlabs_tts_device_test
  elevenlabs_tts_device_test.cpp
)
target_link_libraries(elevenlabs_tts_device_test
  PRIVATE shizuru_tts_elevenlabs_device
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(elevenlabs_tts_device_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
)
add_test(NAME elevenlabs_tts_device_test COMMAND elevenlabs_tts_device_test)
set_tests_properties(elevenlabs_tts_device_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# ASR device property tests
add_executable(asr_device_prop_test
  asr_device_prop_test.cpp
)
target_link_libraries(asr_device_prop_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(asr_device_prop_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME asr_device_prop_test COMMAND asr_device_prop_test)
set_tests_properties(asr_device_prop_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")
