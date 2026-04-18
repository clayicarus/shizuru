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
  conversation/item.cpp
  conversation/render.cpp
  controller/controller.cpp
  context/context_strategy.cpp
  policy/policy_layer.cpp
  session/session.cpp
  strategies/llm_observation_filter.cpp
  strategies/llm_observation_aggregator.cpp
  dialogue/default_reducer.cpp
)

target_include_directories(shizuru_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(shizuru_core
  PUBLIC shizuru_utils
  PUBLIC nlohmann_json::nlohmann_json
)
