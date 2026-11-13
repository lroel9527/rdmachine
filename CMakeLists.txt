cmake_minimum_required(VERSION 3.21)

# Phase 1 targets macOS 12.3+ so the 2016 Intel MacBook Pro (macOS 12) can be
# tested. rdaccel supports 12.3 from 1.0.1 (lroel9527/rdaccel#39).
# Set before project() so it also applies to the rdaccel submodule.
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.3" CACHE STRING "Minimum macOS version")
endif()

project(rdmachine VERSION 3.2.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
endif()

option(RDM_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
# The server and client need rdaccel (macOS: system frameworks; Linux: FFmpeg,
# X11 and PAM development packages). The protocol core does not.
option(RDM_WITH_RDACCEL "Build rdaccel, the session library and the server/client" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT MSVC)
  add_compile_options(-Wall -Wextra)
  # Keep the build machine's folders out of the binaries (__FILE__ in
  # dependencies such as Mbed TLS, debug info): paths become relative.
  add_compile_options(-ffile-prefix-map=${CMAKE_BINARY_DIR}/=
                      -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=)
endif()

include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# TLS 1.3 for the control connection (docs/protocol.md). Mbed TLS builds the
# same way on macOS (universal) and Linux, with no system dependency, and is
# Apache-2.0 like this project.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  URL_HASH SHA256=8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(mbedtls)

add_subdirectory(core)

if(RDM_WITH_RDACCEL)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rdaccel/CMakeLists.txt)
    message(FATAL_ERROR "rdaccel submodule missing: run `git submodule update --init`")
  endif()
  set(RDACCEL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(RDACCEL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(RDACCEL_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/rdaccel EXCLUDE_FROM_ALL)
  add_subdirectory(session)
  add_subdirectory(server)
  add_subdirectory(client)
endif()

if(RDM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/tests)
  if(RDM_WITH_RDACCEL)
    add_subdirectory(session/tests)
  endif()
endif()
