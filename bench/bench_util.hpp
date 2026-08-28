#pragma once

#include <benchmark/benchmark.h>

#include <cstdint>

namespace wsld::bench {

// iterations() * count without sign-conversion warnings: multiply in unsigned,
// then convert to the int64 that SetItemsProcessed/SetBytesProcessed expect.
inline std::int64_t scaled(benchmark::IterationCount iters, std::uint64_t per) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(iters) * per);
}

}  // namespace wsld::bench
