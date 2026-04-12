include(FetchContent)

# Google Test — declared here only if tests/agent hasn't already declared it.
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

# RouteTable tests (unit + property)
add_executable(route_table_test
  route_table_test.cpp
  route_table_prop_test.cpp
)
target_link_libraries(route_table_test
  PRIVATE shizuru_runtime
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(route_table_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME route_table_test COMMAND route_table_test)
set_tests_properties(route_table_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# CoreDevice tests (unit + property)
add_executable(core_device_test
  core_device_test.cpp
  core_device_prop_test.cpp
)
target_link_libraries(core_device_test
  PRIVATE shizuru_runtime
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(core_device_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/tests/agent/mocks
)
add_test(NAME core_device_test COMMAND core_device_test)
set_tests_properties(core_device_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# httplib is used only for the mock HTTP server in tests, not as a production dependency.
FetchContent_Declare(
  httplib
  URL      https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.1.tar.gz
)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(httplib)

# AgentRuntime tests (unit + property)
add_executable(agent_runtime_test
  agent_runtime_test.cpp
  agent_runtime_prop_test.cpp
)
target_link_libraries(agent_runtime_test
  PRIVATE shizuru_runtime
  PRIVATE httplib::httplib
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(agent_runtime_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/tests/runtime
)
add_test(NAME agent_runtime_test COMMAND agent_runtime_test)
set_tests_properties(agent_runtime_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# ToolDispatchDevice tests (unit + property)
add_executable(tool_dispatch_device_test
  tool_dispatch_device_test.cpp
)
target_link_libraries(tool_dispatch_device_test
  PRIVATE shizuru_runtime
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(tool_dispatch_device_test
  PRIVATE ${CMAKE_SOURCE_DIR}
)
add_test(NAME tool_dispatch_device_test COMMAND tool_dispatch_device_test)
set_tests_properties(tool_dispatch_device_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=100")

# AgentRuntime bug condition exploration test (Bug 3)
add_executable(agent_runtime_bug_condition_test
  agent_runtime_bug_condition_test.cpp
)
target_link_libraries(agent_runtime_bug_condition_test
  PRIVATE shizuru_runtime
  PRIVATE httplib::httplib
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(agent_runtime_bug_condition_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/tests/runtime
)
add_test(NAME agent_runtime_bug_condition_test COMMAND agent_runtime_bug_condition_test)
set_tests_properties(agent_runtime_bug_condition_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=5")

# AgentRuntime integration tests (full pipeline, interrupt, tool call round-trip)
add_executable(agent_runtime_integration_test
  agent_runtime_integration_test.cpp
)
target_link_libraries(agent_runtime_integration_test
  PRIVATE shizuru_runtime
  PRIVATE httplib::httplib
  PRIVATE nlohmann_json::nlohmann_json
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(agent_runtime_integration_test
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/tests/runtime
)
add_test(NAME agent_runtime_integration_test COMMAND agent_runtime_integration_test)
