add_library(shizuru_runtime STATIC
  agent_runtime.cpp
)

target_include_directories(shizuru_runtime
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(shizuru_runtime
  PUBLIC shizuru_core
  PUBLIC shizuru_llm
  PUBLIC shizuru_io_services
)
