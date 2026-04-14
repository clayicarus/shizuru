# app/ — Shizuru product layer
#
# Sits above core/, runtime/, io/, services/.
# Contains all product-specific logic: session assembly, memory persistence,
# scheduling, followup management, tool registration, persona prompts.

add_library(shizuru_app STATIC
  assembly/app_runtime.cpp
  memory/sqlite_memory_store.cpp
  scheduler/scheduler_device.cpp
  tools/builtin_tools.cpp
  persona/persona.cpp
)

target_include_directories(shizuru_app
  PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(shizuru_app
  PUBLIC shizuru_runtime
  PUBLIC shizuru_llm_openai
  PUBLIC shizuru_core
)
