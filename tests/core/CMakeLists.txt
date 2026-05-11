include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Smoke tests — Phase 0 capability verification
add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test
  PRIVATE shizuru_core
  PRIVATE shizuru_llm_openai
  PRIVATE GTest::gtest_main
)
target_include_directories(smoke_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
)
add_test(NAME smoke_test COMMAND smoke_test)

# ConversationItem constraint tests
add_executable(conversation_item_test conversation_item_test.cpp)
target_link_libraries(conversation_item_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
)
target_include_directories(conversation_item_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME conversation_item_test COMMAND conversation_item_test)

# Provider render tests
add_executable(provider_render_test provider_render_test.cpp)
target_link_libraries(provider_render_test
  PRIVATE shizuru_core
  PRIVATE shizuru_llm_openai
  PRIVATE GTest::gtest_main
)
target_include_directories(provider_render_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/services
)
add_test(NAME provider_render_test COMMAND provider_render_test)

# Batching tests
add_executable(batching_test batching_test.cpp)
target_link_libraries(batching_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
)
target_include_directories(batching_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME batching_test COMMAND batching_test)

# Tool result flow tests
add_executable(tool_result_flow_test tool_result_flow_test.cpp)
target_link_libraries(tool_result_flow_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
)
target_include_directories(tool_result_flow_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME tool_result_flow_test COMMAND tool_result_flow_test)
