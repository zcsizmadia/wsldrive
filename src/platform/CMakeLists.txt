add_library(wsldrive_platform STATIC)
add_library(wsldrive::platform ALIAS wsldrive_platform)

if(WIN32)
  target_sources(wsldrive_platform PRIVATE
    win/dir_watcher.cpp
    win/wide.cpp
    win/wsl_launch.cpp)
else()
  target_sources(wsldrive_platform PRIVATE
    linux/dir_watcher.cpp
    linux/win_launch.cpp)
endif()

target_link_libraries(wsldrive_platform PUBLIC wsldrive::core)
