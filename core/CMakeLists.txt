include(FetchContent)

if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(
    nlohmann_json
    URL      https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# Core agent framework library
add_library(shizuru_core STATIC
  controller/controller.cpp
  context/context_strategy.cpp
  policy/policy_layer.cpp
  session/session.cpp
  dialogue/default_reducer.cpp
  dialogue/timer_book.cpp
  batching.cpp
  output_interpreter.cpp

  # Header-only semantic pipeline types (listed for IDE visibility)
  content_part.h
  conversation_item.h
  invoke_batch.h
  control_signal.h
  batching.h
  output_interpreter.h
)

target_include_directories(shizuru_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(shizuru_core
  PUBLIC shizuru_utils
  PUBLIC nlohmann_json::nlohmann_json
)
