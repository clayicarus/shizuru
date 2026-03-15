include(FetchContent)

# Google Test (no-op if already fetched by tests/agent)
FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# JSON parser tests
add_executable(json_parser_test
  json_parser_test.cpp
)
target_link_libraries(json_parser_test
  PRIVATE shizuru_llm
  PRIVATE GTest::gtest_main
)
add_test(NAME json_parser_test COMMAND json_parser_test)

# Tool dispatcher tests
add_executable(tool_dispatcher_test
  tool_dispatcher_test.cpp
)
target_link_libraries(tool_dispatcher_test
  PRIVATE shizuru_io_services
  PRIVATE GTest::gtest_main
)
add_test(NAME tool_dispatcher_test COMMAND tool_dispatcher_test)

# OpenAI client tests (uses httplib::Server as mock)
add_executable(openai_client_test
  openai_client_test.cpp
)
target_link_libraries(openai_client_test
  PRIVATE shizuru_llm
  PRIVATE httplib::httplib
  PRIVATE GTest::gtest_main
)
target_include_directories(openai_client_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)
add_test(NAME openai_client_test COMMAND openai_client_test)
