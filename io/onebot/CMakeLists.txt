# io/onebot/CMakeLists.txt — OneBot 11 reverse WebSocket IO device.

include(FetchContent)

FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
)

# IXWebSocket options: disable TLS for simplicity (OneBot is typically local).
# Enable if your OneBot endpoint uses wss://.
set(USE_TLS OFF CACHE BOOL "Disable TLS in IXWebSocket" FORCE)
set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(ixwebsocket)

add_library(shizuru_io_onebot STATIC
  onebot_device.cpp
)

target_include_directories(shizuru_io_onebot PUBLIC
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/utils
)

target_link_libraries(shizuru_io_onebot PUBLIC
  ixwebsocket
  shizuru_core
)
