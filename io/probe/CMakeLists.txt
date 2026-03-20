add_library(shizuru_io_probe STATIC
  log_device.cpp
)

target_include_directories(shizuru_io_debug
  PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(shizuru_io_debug
  PUBLIC shizuru_utils
)
