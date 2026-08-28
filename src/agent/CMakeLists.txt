add_library(wsldrive_agent STATIC
  client.cpp
  scanner.cpp
  server.cpp)
add_library(wsldrive::agent ALIAS wsldrive_agent)

target_link_libraries(wsldrive_agent PUBLIC wsldrive::core wsldrive::net wsldrive::platform)
