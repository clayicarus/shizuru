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

# Controller tests (unit + property)
add_executable(controller_test
  controller_test.cpp
  controller_prop_test.cpp
)
target_link_libraries(controller_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(controller_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
add_test(NAME controller_test COMMAND controller_test)
set_tests_properties(controller_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=5")

# Context strategy tests (unit + property)
add_executable(context_strategy_test
  context_strategy_test.cpp
  context_strategy_prop_test.cpp
)
target_link_libraries(context_strategy_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(context_strategy_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
add_test(NAME context_strategy_test COMMAND context_strategy_test)
set_tests_properties(context_strategy_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# Policy layer tests (unit + property)
add_executable(policy_layer_test
  policy_layer_test.cpp
  policy_layer_prop_test.cpp
)
target_link_libraries(policy_layer_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(policy_layer_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
add_test(NAME policy_layer_test COMMAND policy_layer_test)
set_tests_properties(policy_layer_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=10")

# Strategy tests (ObservationFilter, TtsSegmentStrategy, ResponseFilter)
add_executable(strategy_test
  strategy_test.cpp
)
target_link_libraries(strategy_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
)
target_include_directories(strategy_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
add_test(NAME strategy_test COMMAND strategy_test)
