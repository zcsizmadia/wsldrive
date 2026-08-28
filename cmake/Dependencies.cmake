include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- BLAKE3 -----------------------------------------------------------------
# Official C implementation; the CMake project lives in the `c/` subdirectory.
FetchContent_Declare(blake3
  URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.5.5.tar.gz
  URL_HASH SHA256=6feba0750efc1a99a79fb9a495e2628b5cd1603e15f56a06b1d6cb13ac55c618
  SOURCE_SUBDIR c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# --- GoogleTest -------------------------------------------------------------
if(WSLDRIVE_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
endif()

# --- Google Benchmark ---------------------------------------------------------
if(WSLDRIVE_BUILD_BENCHMARKS)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    URL_HASH SHA256=32131c08ee31eeff2c8968d7e874f3cb648034377dfc32a4c377fa8796d84981
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
endif()

set(_wsld_deps blake3)
if(WSLDRIVE_BUILD_TESTS)
  list(APPEND _wsld_deps googletest)
endif()
if(WSLDRIVE_BUILD_BENCHMARKS)
  list(APPEND _wsld_deps benchmark)
endif()
FetchContent_MakeAvailable(${_wsld_deps})

# BLAKE3 exports the `blake3` target with its include directory already on the
# build interface. `blake3_SOURCE_DIR` is kept for consumers that need the header
# path explicitly (e.g. when the upstream target's interface is install-only).
set(WSLDRIVE_BLAKE3_INCLUDE_DIR "${blake3_SOURCE_DIR}/c")
