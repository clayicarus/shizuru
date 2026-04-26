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

# Controller tests (unit + property + Phase 2 regression)
add_executable(controller_test
  controller_test.cpp
  controller_prop_test.cpp
  controller_phase2_test.cpp
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

# Controller bug condition exploration tests (Bug 1 + Bug 2)
add_executable(controller_bug_condition_test
  controller_bug_condition_test.cpp
)
target_link_libraries(controller_bug_condition_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(controller_bug_condition_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks
)
add_test(NAME controller_bug_condition_test COMMAND controller_bug_condition_test)
set_tests_properties(controller_bug_condition_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=5")

# Dialogue reducer tests (unit + property)
add_executable(dialogue_reducer_test
  dialogue_reducer_test.cpp
  dialogue_reducer_phase2_test.cpp
  dialogue_reducer_phase3_test.cpp
  dialogue_reducer_prop_test.cpp
)
target_link_libraries(dialogue_reducer_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(dialogue_reducer_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME dialogue_reducer_test COMMAND dialogue_reducer_test)
set_tests_properties(dialogue_reducer_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=100")

# TimerBook tests (unit)
add_executable(timer_book_test
  timer_book_test.cpp
)
target_link_libraries(timer_book_test
  PRIVATE shizuru_core
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(timer_book_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(NAME timer_book_test COMMAND timer_book_test)
set_tests_properties(timer_book_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=100")

# Controller preservation tests (must pass on unfixed code)
add_executable(controller_preservation_test
  controller_preservation_test.cpp
)
target_link_libraries(controller_preservation_test
  PRIVATE shizuru_runtime
  PRIVATE GTest::gtest_main
  PRIVATE rapidcheck_gtest
)
target_include_directories(controller_preservation_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  PRIVATE ${CMAKE_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/tests/runtime
)
add_test(NAME controller_preservation_test COMMAND controller_preservation_test)
set_tests_properties(controller_preservation_test PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=5")
