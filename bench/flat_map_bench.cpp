#include "bench_util.hpp"
#include "core/flat_map.hpp"

#include <benchmark/benchmark.h>

#include <random>
#include <unordered_map>
#include <vector>

namespace wsld::bench {
namespace {

std::vector<std::uint64_t> keys(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<std::uint64_t> out(n);
  for (auto& k : out) k = rng() & 0x0000FFFFFFFFFFFFULL;  // stays clear of the sentinels
  return out;
}

void BM_U64MapInsert(benchmark::State& state) {
  const auto ks = keys(static_cast<std::size_t>(state.range(0)), 1);
  for (auto _ : state) {
    U64Map m;
    for (std::size_t i = 0; i < ks.size(); ++i) m.insert_or_assign(ks[i], static_cast<std::uint32_t>(i));
    benchmark::DoNotOptimize(m.size());
  }
  state.SetItemsProcessed(scaled(state.iterations(), ks.size()));
}
BENCHMARK(BM_U64MapInsert)->Arg(1 << 10)->Arg(1 << 17)->Arg(1 << 20)->Unit(benchmark::kMicrosecond);

void BM_StdUnorderedMapInsert(benchmark::State& state) {
  const auto ks = keys(static_cast<std::size_t>(state.range(0)), 1);
  for (auto _ : state) {
    std::unordered_map<std::uint64_t, std::uint32_t> m;
    for (std::size_t i = 0; i < ks.size(); ++i) m.insert_or_assign(ks[i], static_cast<std::uint32_t>(i));
    benchmark::DoNotOptimize(m.size());
  }
  state.SetItemsProcessed(scaled(state.iterations(), ks.size()));
}
BENCHMARK(BM_StdUnorderedMapInsert)->Arg(1 << 10)->Arg(1 << 17)->Arg(1 << 20)->Unit(benchmark::kMicrosecond);

void BM_U64MapFindHit(benchmark::State& state) {
  const auto ks = keys(static_cast<std::size_t>(state.range(0)), 1);
  U64Map m;
  for (std::size_t i = 0; i < ks.size(); ++i) m.insert_or_assign(ks[i], static_cast<std::uint32_t>(i));
  std::size_t i = 0;
  const std::size_t mask = ks.size() - 1;
  for (auto _ : state) {
    auto* v = m.find(ks[i++ & mask]);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_U64MapFindHit)->Arg(1 << 10)->Arg(1 << 17)->Arg(1 << 20);

void BM_StdUnorderedMapFindHit(benchmark::State& state) {
  const auto ks = keys(static_cast<std::size_t>(state.range(0)), 1);
  std::unordered_map<std::uint64_t, std::uint32_t> m;
  for (std::size_t i = 0; i < ks.size(); ++i) m.insert_or_assign(ks[i], static_cast<std::uint32_t>(i));
  std::size_t i = 0;
  const std::size_t mask = ks.size() - 1;
  for (auto _ : state) {
    auto it = m.find(ks[i++ & mask]);
    benchmark::DoNotOptimize(it);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdUnorderedMapFindHit)->Arg(1 << 10)->Arg(1 << 17)->Arg(1 << 20);

void BM_U64MapFindMiss(benchmark::State& state) {
  const auto ks = keys(static_cast<std::size_t>(state.range(0)), 1);
  const auto misses = keys(4096, 2);
  U64Map m;
  for (std::size_t i = 0; i < ks.size(); ++i) m.insert_or_assign(ks[i], static_cast<std::uint32_t>(i));
  std::size_t i = 0;
  for (auto _ : state) {
    auto* v = m.find(misses[i++ & 4095]);
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_U64MapFindMiss)->Arg(1 << 17)->Arg(1 << 20);

}  // namespace
}  // namespace wsld::bench
