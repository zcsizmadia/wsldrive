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
