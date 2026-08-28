add_library(wsldrive_mount STATIC fuse_mount.cpp)
add_library(wsldrive::mount ALIAS wsldrive_mount)

# winfsp::fuse3 is PUBLIC so WSLDRIVE_HAVE_WINFSP and the FUSE headers propagate
# to whatever links the mount library (the wsldrive CLI).
target_link_libraries(wsldrive_mount
  PUBLIC wsldrive::agent winfsp::fuse3
  PRIVATE wsldrive::options advapi32)
