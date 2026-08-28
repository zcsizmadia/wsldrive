add_executable(core_bench
  coalescer_bench.cpp
  flat_map_bench.cpp
  hash_bench.cpp
  protocol_bench.cpp
  tree_bench.cpp)

target_link_libraries(core_bench PRIVATE wsldrive::core benchmark::benchmark_main)
