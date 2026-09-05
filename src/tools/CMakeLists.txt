add_executable(wsldrived wsldrived_main.cpp)
target_link_libraries(wsldrived PRIVATE wsldrive::agent)

add_executable(wsldrive wsldrive_main.cpp)
target_link_libraries(wsldrive PRIVATE wsldrive::agent)
if(TARGET wsldrive_mount)
  target_link_libraries(wsldrive PRIVATE wsldrive::mount)
endif()

# Windowless launcher (GUI subsystem, like pythonw.exe): the at-logon mount task
# runs the console mount through this so no window appears. Windows only.
if(WIN32)
  add_executable(wsldrivew WIN32 wsldrivew_main.cpp)
endif()

# Stamp each executable with a VERSIONINFO resource, from the same
# PROJECT_VERSION the binaries print. Without it Explorer's Details tab and
# (Get-Item wsldrive.exe).VersionInfo say nothing about which build this is.
if(WIN32)
  enable_language(RC)
  foreach(tgt wsldrive wsldrived wsldrivew)
    if(TARGET ${tgt})
      set(WSLDRIVE_RC_NAME "${tgt}")
      if(tgt STREQUAL "wsldrived")
        set(WSLDRIVE_RC_DESCRIPTION "wsldrive agent (serves a directory tree)")
      elseif(tgt STREQUAL "wsldrivew")
        set(WSLDRIVE_RC_DESCRIPTION "wsldrive windowless launcher")
      else()
        set(WSLDRIVE_RC_DESCRIPTION "wsldrive command line tool")
      endif()
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
                     "${CMAKE_CURRENT_BINARY_DIR}/${tgt}_version.rc" @ONLY)
      target_sources(${tgt} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${tgt}_version.rc")
    endif()
  endforeach()
endif()
