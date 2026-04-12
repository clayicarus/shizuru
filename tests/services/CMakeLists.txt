include(FetchContent)

# Google Test (no-op if already fetched by tests/agent)
FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# JSON parser tests
add_executable(json_parser_test
  json_parser_test.cpp
)
target_link_libraries(json_parser_test
  PRIVATE shizuru_llm_openai
  PRIVATE GTest::gtest_main
)
add_test(NAME json_parser_test COMMAND json_parser_test)

# ToolRegistry tests
add_executable(tool_dispatcher_test
  tool_dispatcher_test.cpp
)
target_link_libraries(tool_dispatcher_test
  PRIVATE shizuru_io_services
  PRIVATE GTest::gtest_main
)
add_test(NAME tool_dispatcher_test COMMAND tool_dispatcher_test)

# OpenAI client tests (uses httplib::Server as mock)
# httplib is used only for the mock HTTP server, not as a production dependency.
FetchContent_Declare(
  httplib
  URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

add_executable(openai_client_test
  openai_client_test.cpp
)
target_link_libraries(openai_client_test
  PRIVATE shizuru_llm_openai
  PRIVATE httplib::httplib
  PRIVATE GTest::gtest_main
)
target_include_directories(openai_client_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)
add_test(NAME openai_client_test COMMAND openai_client_test)

# RapidCheck (for TTS/ASR device property tests)
set(RC_ENABLE_GTEST ON CACHE BOOL "Build RapidCheck Google Test integration" FORCE)
FetchContent_Declare(
  rapidcheck
  URL      https://github.com/emil-e/rapidcheck/archive/refs/heads/master.tar.gz
)
FetchContent_MakeAvailable(rapidcheck)

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
