# Locates libfuse3 (Linux, for the Direction B mount) via pkg-config and, if
# found, defines an imported interface target wsldrive::fuse3. Absent (or on
# Windows), leaves WSLDRIVE_FUSE_FOUND false and the Linux mount is skipped.

set(WSLDRIVE_FUSE_FOUND FALSE)

if(NOT WIN32)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
    if(FUSE3_FOUND)
      set(WSLDRIVE_FUSE_FOUND TRUE)
      add_library(wsldrive_fuse3 INTERFACE)
      add_library(wsldrive::fuse3 ALIAS wsldrive_fuse3)
      target_link_libraries(wsldrive_fuse3 INTERFACE PkgConfig::FUSE3)
      message(STATUS "libfuse3 ${FUSE3_VERSION} found (Direction B mount enabled)")
    endif()
  endif()
  if(NOT WSLDRIVE_FUSE_FOUND)
    message(STATUS "libfuse3 not found (Linux mount disabled; install libfuse3-dev)")
  endif()
endif()
