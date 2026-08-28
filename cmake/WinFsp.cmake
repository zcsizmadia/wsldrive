# Locates a WinFsp installation (runtime + Developer SDK) and, if found, defines
# an imported interface target `winfsp::fuse3` carrying the FUSE3 include dir,
# the x64 import library, and the WSLDRIVE_HAVE_WINFSP compile definition.
#
# Not found (e.g. CI runners without WinFsp) simply leaves WSLDRIVE_WINFSP_FOUND
# false and the mount target is skipped — the rest of the build is unaffected.

set(WSLDRIVE_WINFSP_FOUND FALSE)

if(WIN32)
  set(_winfsp_candidates
    "$ENV{WINFSP}"
    "C:/Program Files (x86)/WinFsp"
    "C:/Program Files/WinFsp")
  foreach(_root IN LISTS _winfsp_candidates)
    if(_root AND EXISTS "${_root}/inc/fuse3/fuse.h" AND EXISTS "${_root}/lib/winfsp-x64.lib")
      set(WSLDRIVE_WINFSP_ROOT "${_root}")
      break()
    endif()
  endforeach()

  if(DEFINED WSLDRIVE_WINFSP_ROOT)
    set(WSLDRIVE_WINFSP_FOUND TRUE)
    add_library(winfsp_fuse3 INTERFACE)
    add_library(winfsp::fuse3 ALIAS winfsp_fuse3)
    # SYSTEM include so the third-party headers do not trip our /W4 warnings.
    target_include_directories(winfsp_fuse3 SYSTEM INTERFACE "${WSLDRIVE_WINFSP_ROOT}/inc")
    # Delay-load the WinFsp DLL and locate it at runtime via FspLoad() (registry),
    # so the binary runs without WinFsp's bin directory on PATH.
    target_link_libraries(winfsp_fuse3 INTERFACE "${WSLDRIVE_WINFSP_ROOT}/lib/winfsp-x64.lib" delayimp)
    target_link_options(winfsp_fuse3 INTERFACE "/DELAYLOAD:winfsp-x64.dll")
    target_compile_definitions(winfsp_fuse3 INTERFACE WSLDRIVE_HAVE_WINFSP FUSE_USE_VERSION=31)
    message(STATUS "WinFsp found at ${WSLDRIVE_WINFSP_ROOT} (mount target enabled)")
  endif()
endif()

if(NOT WSLDRIVE_WINFSP_FOUND)
  message(STATUS "WinFsp not found (mount target disabled; set WINFSP env to override)")
endif()
