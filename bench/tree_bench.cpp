#include "bench_util.hpp"
#include "core/metadata_tree.hpp"
#include "synthetic.hpp"

#include <benchmark/benchmark.h>

#include <random>

namespace wsld::bench {
namespace {

// 100 x 10 x 100 = 100k files + 1.1k directories: a mid-sized monorepo.
const SyntheticTree& big() {
  static const SyntheticTree s = SyntheticTree::build(100, 10, 100);
  return s;
}

void BM_TreeBuild100k(benchmark::State& state) {
  for (auto _ : state) {
    SyntheticTree s = SyntheticTree::build(100, 10, 100);
    benchmark::DoNotOptimize(s.tree.size());
  }
  state.SetItemsProcessed(state.iterations() * 101100);
}
BENCHMARK(BM_TreeBuild100k)->Unit(benchmark::kMillisecond);

void BM_LookupExactHit(benchmark::State& state) {
  const SyntheticTree& s = big();
  std::mt19937 rng(1);
  std::vector<const std::string*> probes;
  for (int i = 0; i < 4096; ++i) probes.push_back(&s.file_paths[rng() % s.file_paths.size()]);
  std::size_t i = 0;
  for (auto _ : state) {
    auto r = s.tree.lookup(*probes[i++ & 4095], LookupMode::Exact);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LookupExactHit);

void BM_LookupCaseInsensitiveExactCaseHit(benchmark::State& state) {
  // Windows clients ask case-insensitively but usually with the right case:
  // this must be as fast as the exact path.
  const SyntheticTree& s = big();
  std::mt19937 rng(2);
  std::vector<const std::string*> probes;
  for (int i = 0; i < 4096; ++i) probes.push_back(&s.file_paths[rng() % s.file_paths.size()]);
  std::size_t i = 0;
  for (auto _ : state) {
    auto r = s.tree.lookup(*probes[i++ & 4095], LookupMode::CaseInsensitive);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LookupCaseInsensitiveExactCaseHit);

void BM_LookupCaseInsensitiveWrongCaseHit(benchmark::State& state) {
  const SyntheticTree& s = big();
  std::mt19937 rng(3);
  std::vector<std::string> probes;
  for (int i = 0; i < 4096; ++i) probes.push_back(lowered(s.file_paths[rng() % s.file_paths.size()]));
  std::size_t i = 0;
  for (auto _ : state) {
    auto r = s.tree.lookup(probes[i++ & 4095], LookupMode::CaseInsensitive);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LookupCaseInsensitiveWrongCaseHit);

void BM_LookupNegativeUnknownName(benchmark::State& state) {
  // Windows tooling probes many names that exist nowhere (e.g. "desktop.ini").
  const SyntheticTree& s = big();
  std::mt19937 rng(4);
  std::vector<std::string> probes;
  for (int i = 0; i < 4096; ++i) probes.push_back(s.dir_paths[rng() % s.dir_paths.size()] + "/desktop.ini");
  std::size_t i = 0;
  for (auto _ : state) {
    auto r = s.tree.lookup(probes[i++ & 4095], LookupMode::CaseInsensitive);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LookupNegativeUnknownName);

void BM_LookupNegativeKnownNameWrongDir(benchmark::State& state) {
  // The name exists elsewhere, so the interner cannot short-circuit.
  const SyntheticTree& s = big();
  std::mt19937 rng(5);
  std::vector<std::string> probes;
  for (int i = 0; i < 4096; ++i) probes.push_back(s.dir_paths[rng() % 100] + "/Source_File_1.cpp");
  std::size_t i = 0;
  for (auto _ : state) {
    auto r = s.tree.lookup(probes[i++ & 4095], LookupMode::CaseInsensitive);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LookupNegativeKnownNameWrongDir);

void BM_Readdir100Entries(benchmark::State& state) {
  const SyntheticTree& s = big();
  const NodeId dir = *s.tree.lookup("module_7/component3");
  for (auto _ : state) {
    std::uint64_t total = 0;
    s.tree.for_each_child(dir, [&](NodeId c) {
      const auto& n = s.tree.node(c);
      total += n.attr.size + s.tree.name(c).size();
    });
    benchmark::DoNotOptimize(total);
  }
  state.SetItemsProcessed(state.iterations() * 100);
}
BENCHMARK(BM_Readdir100Entries);

void BM_UpsertPathExisting(benchmark::State& state) {
  SyntheticTree s = SyntheticTree::build(20, 10, 100);
  std::mt19937 rng(6);
  std::size_t i = 0;
  Attributes a = kFileAttr;
  for (auto _ : state) {
    a.size = i;
    auto r = s.tree.upsert_path(s.file_paths[(i++ * 7919) % s.file_paths.size()], a);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_UpsertPathExisting);

void BM_InsertRemoveChurn(benchmark::State& state) {
  SyntheticTree s = SyntheticTree::build(20, 10, 100);
  const NodeId dir = *s.tree.lookup("module_1/component1");
  for (auto _ : state) {
    const NodeId id = *s.tree.insert(dir, "tmp_build_artifact.o", kFileAttr);
    (void)s.tree.remove(id);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_InsertRemoveChurn);

void BM_ExportSnapshot100k(benchmark::State& state) {
  const SyntheticTree& s = big();
  for (auto _ : state) {
    std::uint64_t total = 0;
    s.tree.export_snapshot([&](const SnapshotEntry& e) { total += e.parent + e.name.size(); });
    benchmark::DoNotOptimize(total);
  }
  state.SetItemsProcessed(scaled(state.iterations(), big().tree.size() - 1));
}
BENCHMARK(BM_ExportSnapshot100k)->Unit(benchmark::kMillisecond);

void BM_LoadSnapshot100k(benchmark::State& state) {
  const SyntheticTree& s = big();
  std::vector<SnapshotEntry> entries;
  s.tree.export_snapshot([&](const SnapshotEntry& e) { entries.push_back(e); });
  for (auto _ : state) {
    MetadataTree t;
    (void)t.load_snapshot(entries);
    benchmark::DoNotOptimize(t.size());
  }
  state.SetItemsProcessed(scaled(state.iterations(), entries.size()));
}
BENCHMARK(BM_LoadSnapshot100k)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace wsld::bench
