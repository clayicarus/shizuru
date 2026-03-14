add_library(shizuru_io_services STATIC
  tool_dispatcher.cpp
)

target_include_directories(shizuru_io_services
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(shizuru_io_services
  PUBLIC shizuru_core
)
