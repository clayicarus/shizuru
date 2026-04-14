add_library(shizuru_runtime STATIC
  agent_runtime.cpp
  core_device.cpp
  tool_dispatch_device.cpp
)

target_include_directories(shizuru_runtime
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(shizuru_runtime
  PUBLIC shizuru_core
  PUBLIC shizuru_utils
)
