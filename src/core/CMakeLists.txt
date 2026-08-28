add_library(wsldrive_core STATIC
  coalescer.cpp
  content_cache.cpp
  hash.cpp
  metadata_tree.cpp
  path.cpp
  protocol.cpp
  string_pool.cpp
  unicode.cpp)
add_library(wsldrive::core ALIAS wsldrive_core)

target_include_directories(wsldrive_core
  PUBLIC "${CMAKE_SOURCE_DIR}/src"
  PRIVATE "${WSLDRIVE_BLAKE3_INCLUDE_DIR}")
target_link_libraries(wsldrive_core
  PUBLIC wsldrive::options
  PRIVATE blake3)
