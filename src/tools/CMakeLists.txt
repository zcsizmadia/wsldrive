add_executable(wsldrived wsldrived_main.cpp)
target_link_libraries(wsldrived PRIVATE wsldrive::agent)

add_executable(wsldrive wsldrive_main.cpp)
target_link_libraries(wsldrive PRIVATE wsldrive::agent)
if(WSLDRIVE_WINFSP_FOUND)
  target_link_libraries(wsldrive PRIVATE wsldrive::mount)
endif()
