include(FetchContent)

# ── libcurl ──────────────────────────────────────────────────────────
# On Android, build curl with BoringSSL.  On other platforms, use the
# system OpenSSL / SecureTransport that find_package discovers.
if(ANDROID)
  # BoringSSL — Google's OpenSSL fork, native to Android.
  FetchContent_Declare(
    boringssl
    GIT_REPOSITORY https://boringssl.googlesource.com/boringssl
    GIT_TAG        main
    GIT_SHALLOW    TRUE
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

  # Populate BoringSSL source but add it with EXCLUDE_FROM_ALL so that
  # only the targets we explicitly link (ssl, crypto) are built.
  # This prevents BoringSSL's test/tool executables (e.g. "resolve")
  # from colliding with identically-named curl test targets.
  FetchContent_GetProperties(boringssl)
  if(NOT boringssl_POPULATED)
    FetchContent_Populate(boringssl)
    add_subdirectory(${boringssl_SOURCE_DIR} ${boringssl_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()

  # BoringSSL creates 'ssl' and 'crypto' targets.
  if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL    ALIAS ssl)
    add_library(OpenSSL::Crypto ALIAS crypto)
  endif()

  set(OPENSSL_INCLUDE_DIR "${boringssl_SOURCE_DIR}/include" CACHE PATH "" FORCE)
  set(OPENSSL_CRYPTO_LIBRARY crypto CACHE STRING "" FORCE)
  set(OPENSSL_SSL_LIBRARY ssl CACHE STRING "" FORCE)
  set(OPENSSL_FOUND TRUE)
  set(OpenSSL_FOUND TRUE)

  # Tell curl to use our BoringSSL.
  set(CURL_USE_OPENSSL ON  CACHE BOOL "" FORCE)
  set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
  set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
  set(CURL_USE_WOLFSSL OFF CACHE BOOL "" FORCE)

  # Don't embed a CA bundle/path at build time — we set CURLOPT_CAPATH
  # at runtime to point to the Android system CA directory.
  set(CURL_CA_BUNDLE  "none" CACHE STRING "" FORCE)
  set(CURL_CA_PATH    "none" CACHE STRING "" FORCE)

  set(SHIZURU_ANDROID_SSL TRUE)
else()
  # Desktop / iOS: let curl find the system SSL.
  # macOS: SecureTransport or Homebrew OpenSSL.
  # Linux: system OpenSSL.
  if(APPLE AND NOT IOS)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
  endif()
endif()

# Disable curl features we don't need to speed up the build.
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_COOKIES OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
# Disable curl's install/export rules — we only use it as a build-time
# static dependency.  This avoids CMake errors when curl tries to export
# targets that reference BoringSSL's ssl/crypto (which are not in any
# export set).
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  curl
  URL      https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.gz
)
FetchContent_MakeAvailable(curl)

# Provide a single imported target for downstream use.
if(NOT TARGET CURL::libcurl)
  add_library(CURL::libcurl ALIAS libcurl)
endif()

add_subdirectory(utils)
add_subdirectory(llm)
add_subdirectory(asr)
add_subdirectory(io)
add_subdirectory(tts)
