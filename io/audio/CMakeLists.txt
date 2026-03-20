include(FetchContent)

# PortAudio
FetchContent_Declare(
  portaudio
  GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
  GIT_TAG        v19.7.0
)
set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(portaudio)

# Audio library (header-only templates + pa_init + IoDevice wrappers)
add_library(shizuru_audio STATIC
  audio_device/port_audio/pa_init.cpp
  audio_capture_device.cpp
  audio_playout_device.cpp
)

target_include_directories(shizuru_audio
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC  ${CMAKE_SOURCE_DIR}
  PUBLIC  ${portaudio_SOURCE_DIR}/include
)

target_link_libraries(shizuru_audio
  PUBLIC portaudio_static
  PUBLIC shizuru_utils
)
