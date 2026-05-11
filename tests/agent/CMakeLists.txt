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
