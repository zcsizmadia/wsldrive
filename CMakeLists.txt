cmake_minimum_required(VERSION 3.28)

project(wsldrive
  VERSION 0.9.0
  DESCRIPTION "Native-speed file access across the WSL2 boundary"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# One source of truth for the version: PROJECT_VERSION above. Everything that
# needs it reads the generated header rather than repeating the number.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.hpp.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.hpp" @ONLY)

option(WSLDRIVE_BUILD_TESTS "Build unit tests" ON)
option(WSLDRIVE_BUILD_BENCHMARKS "Build micro-benchmarks" ON)
option(WSLDRIVE_WARNINGS_AS_ERRORS "Treat compiler and linker warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)
include(WinFsp)
include(Fuse)

add_subdirectory(src/core)
add_subdirectory(src/net)
add_subdirectory(src/platform)
add_subdirectory(src/agent)
if(WSLDRIVE_WINFSP_FOUND OR WSLDRIVE_FUSE_FOUND)
  add_subdirectory(src/mount)
endif()
add_subdirectory(src/tools)

if(WSLDRIVE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(WSLDRIVE_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
