add_library(wsldrive_net STATIC
  frame_channel.cpp
  socket.cpp)
add_library(wsldrive::net ALIAS wsldrive_net)

target_link_libraries(wsldrive_net PUBLIC wsldrive::core)
if(WIN32)
  target_link_libraries(wsldrive_net PUBLIC ws2_32)
endif()
