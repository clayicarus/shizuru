# Shared utilities (logging, etc.)
add_library(shizuru_utils STATIC
  async_logger.cpp
)

# ── spdlog (async, high-performance logger) ──────────────────────────
include(FetchContent)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.15.1
)
FetchContent_MakeAvailable(spdlog)

target_include_directories(shizuru_utils
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(shizuru_utils
  PUBLIC spdlog::spdlog
)
