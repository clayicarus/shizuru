# app/ — Shizuru product layer
#
# Sits above core/, runtime/, io/, services/.
# Contains all product-specific logic: session assembly, memory persistence,
# scheduling, followup management, tool registration, persona prompts.

# ── SQLite3 ─────────────────────────────────────────────────────────────
# On desktop, prefer the system SQLite3 if available.
# On Android (or when the system package is missing), build the amalgamation
# from source via FetchContent so the build is fully self-contained.
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    sqlite3_amalgamation
    URL      https://www.sqlite.org/2026/sqlite-amalgamation-3530000.zip
  )
  FetchContent_GetProperties(sqlite3_amalgamation)
  if(NOT sqlite3_amalgamation_POPULATED)
    FetchContent_Populate(sqlite3_amalgamation)
  endif()

  add_library(sqlite3_lib STATIC
    ${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c
  )
  target_include_directories(sqlite3_lib
    PUBLIC ${sqlite3_amalgamation_SOURCE_DIR}
  )
  # Silence compiler warnings in third-party C code.
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sqlite3_lib PRIVATE -w)
  endif()
  # Android needs libdl and liblog for SQLite.
  if(ANDROID)
    target_link_libraries(sqlite3_lib PUBLIC dl log)
  endif()

  add_library(SQLite::SQLite3 ALIAS sqlite3_lib)
endif()

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
  PUBLIC SQLite::SQLite3
)
