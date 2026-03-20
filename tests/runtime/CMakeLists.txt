include(FetchContent)

# Google Test — declared here only if tests/agent hasn't already declared it.
FetchContent_Declare(
  googletest
  URL      https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# RapidCheck
set(RC_ENABLE_GTEST ON CACHE BOOL "Build RapidCheck Google Test integration" FORCE)
FetchContent_Declare(
  rapidcheck
  URL      https://github.com/emil-e/rapidcheck/archive/refs/heads/master.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
set_tests_properties(route_table_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=100")

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
set_tests_properties(core_device_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=50")

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
set_tests_properties(agent_runtime_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=20")
