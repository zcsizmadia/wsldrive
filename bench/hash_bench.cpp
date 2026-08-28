#include "core/hash.hpp"
#include "core/hash_util.hpp"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

namespace wsld::bench {
namespace {

void BM_Blake3(benchmark::State& state) {
  const std::vector<std::byte> data(static_cast<std::size_t>(state.range(0)), std::byte{0x5A});
  for (auto _ : state) {
    Digest d = blake3_hash(data);
    benchmark::DoNotOptimize(d);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_Blake3)->Arg(64)->Arg(4096)->Arg(64 << 10)->Arg(1 << 20)->Arg(16 << 20);

void BM_Blake3Incremental64K(benchmark::State& state) {
  const std::vector<std::byte> data(1 << 20, std::byte{0x5A});
  for (auto _ : state) {
    Blake3Hasher h;
    for (std::size_t off = 0; off < data.size(); off += 65536) h.update(std::span(data).subspan(off, 65536));
    Digest d = h.finalize();
    benchmark::DoNotOptimize(d);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(data.size()));
}
BENCHMARK(BM_Blake3Incremental64K);

void BM_HashBytesName(benchmark::State& state) {
  const std::string name(static_cast<std::size_t>(state.range(0)), 'n');
  for (auto _ : state) {
    auto h = hash_string(name);
    benchmark::DoNotOptimize(h);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_HashBytesName)->Arg(8)->Arg(16)->Arg(32)->Arg(64)->Arg(255);

void BM_DigestHex(benchmark::State& state) {
  const Digest d = blake3_hash("x");
  for (auto _ : state) {
    auto hex = d.hex();
    auto back = Digest::from_hex(hex);
    benchmark::DoNotOptimize(back);
  }
}
BENCHMARK(BM_DigestHex);

}  // namespace
}  // namespace wsld::bench
