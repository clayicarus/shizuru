include(FetchContent)

find_package(OpenSSL QUIET)

if(NOT OpenSSL_FOUND AND WIN32)
  FetchContent_Declare(
    openssl_cmake
    GIT_REPOSITORY https://github.com/janbar/openssl-cmake.git
    GIT_TAG        3.3.1-20240911
  )
  FetchContent_MakeAvailable(openssl_cmake)
  if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL    ALIAS ssl)
    add_library(OpenSSL::Crypto ALIAS crypto)
  endif()
  set(OpenSSL_FOUND TRUE)
endif()

if(OpenSSL_FOUND)
  set(HTTPLIB_REQUIRE_OPENSSL ON  CACHE BOOL "" FORCE)
else()
  message(WARNING "OpenSSL not found — HTTPS disabled. "
                  "Use an HTTP endpoint (e.g., http://localhost:11434).")
  set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
endif()

add_subdirectory(llm)
add_subdirectory(baidu)
add_subdirectory(io)
add_subdirectory(tts)
