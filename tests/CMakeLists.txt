add_executable(core_tests
  auth_token_test.cpp
  coalescer_test.cpp
  flat_map_test.cpp
  hash_test.cpp
  ignore_test.cpp
  metadata_tree_test.cpp
  path_test.cpp
  protocol_test.cpp
  string_pool_test.cpp
  unicode_test.cpp
  version_test.cpp
  win_names_test.cpp)

target_link_libraries(core_tests PRIVATE wsldrive::core GTest::gtest_main)

add_executable(agent_tests
  agent_test.cpp
  net_test.cpp)
target_link_libraries(agent_tests PRIVATE wsldrive::agent GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(core_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(agent_tests DISCOVERY_MODE PRE_TEST)
