add_library(wsldrive_core STATIC
  auth_token.cpp
  coalescer.cpp
  hash.cpp
  ignore.cpp
  metadata_tree.cpp
  path.cpp
  protocol.cpp
  string_pool.cpp
  unicode.cpp
  win_names.cpp)
add_library(wsldrive::core ALIAS wsldrive_core)

target_include_directories(wsldrive_core
  PUBLIC "${CMAKE_SOURCE_DIR}/src"
  PRIVATE "${WSLDRIVE_BLAKE3_INCLUDE_DIR}")
target_link_libraries(wsldrive_core
  PUBLIC wsldrive::options
  PRIVATE blake3)
if(WIN32)
  target_link_libraries(wsldrive_core PRIVATE bcrypt)  # BCryptGenRandom for the auth token
endif()
