#include "bench_util.hpp"
#include "core/coalescer.hpp"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

namespace wsld::bench {
namespace {

using namespace std::chrono_literals;

std::vector<std::string> checkout_paths(int n) {
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    out.push_back("src/module_" + std::to_string(i % 50) + "/component" + std::to_string(i % 7) + "/file_" +
                  std::to_string(i) + ".cpp");
  return out;
}

// `git checkout` of a big branch: N distinct files, each Created then Modified twice.
void BM_CoalesceCheckoutBurst(benchmark::State& state) {
  const auto paths = checkout_paths(static_cast<int>(state.range(0)));
  const auto t0 = Coalescer::clock::time_point{} + 1s;
  for (auto _ : state) {
    Coalescer c(Coalescer::Options{.max_pending = 1u << 20, .quiet_period = 1h, .max_latency = 1h});
    for (const auto& p : paths) {
      c.push({FsEventKind::Created, p}, t0);
      c.push({FsEventKind::Modified, p}, t0);
      c.push({FsEventKind::Modified, p}, t0);
    }
    auto ops = c.take();
    benchmark::DoNotOptimize(ops);
  }
  state.SetItemsProcessed(scaled(state.iterations(), paths.size() * 3));
}
BENCHMARK(BM_CoalesceCheckoutBurst)->Arg(1000)->Arg(10000)->Unit(benchmark::kMicrosecond);

// Build output: one file rewritten thousands of times.
void BM_CoalesceHotFile(benchmark::State& state) {
  const auto t0 = Coalescer::clock::time_point{} + 1s;
  Coalescer c(Coalescer::Options{.max_pending = 1u << 20, .quiet_period = 1h, .max_latency = 1h});
  for (auto _ : state) {
    c.push({FsEventKind::Modified, "build/output.log"}, t0);
  }
  benchmark::DoNotOptimize(c.take());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CoalesceHotFile);

// rm -rf of a tree after events on every file inside it.
void BM_CoalesceRemoveTreeCollapse(benchmark::State& state) {
  const auto paths = checkout_paths(10000);
  const auto t0 = Coalescer::clock::time_point{} + 1s;
  for (auto _ : state) {
    Coalescer c(Coalescer::Options{.max_pending = 1u << 20, .quiet_period = 1h, .max_latency = 1h});
    for (const auto& p : paths) c.push({FsEventKind::Modified, p}, t0);
    c.push({FsEventKind::Removed, "src"}, t0);
    auto ops = c.take();
    benchmark::DoNotOptimize(ops);
  }
  state.SetItemsProcessed(scaled(state.iterations(), paths.size() + 1));
}
BENCHMARK(BM_CoalesceRemoveTreeCollapse)->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace wsld::bench
