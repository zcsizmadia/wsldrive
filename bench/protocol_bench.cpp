#include "core/protocol.hpp"
#include "synthetic.hpp"

#include <benchmark/benchmark.h>

namespace wsld::bench {
namespace {

const SyntheticTree& big() {
  static const SyntheticTree s = SyntheticTree::build(100, 10, 100);
  return s;
}

std::vector<std::byte> encoded_snapshot() {
  std::vector<std::byte> buf;
  proto::write_frame(buf, proto::MsgType::Snapshot, 1, [&](proto::Writer& w) {
    proto::write_snapshot_header(w, 1, static_cast<std::uint32_t>(big().tree.size() - 1));
    big().tree.export_snapshot([&](const SnapshotEntry& e) { proto::write_snapshot_entry(w, e); });
  });
  return buf;
}

void BM_EncodeSnapshot100k(benchmark::State& state) {
  std::vector<std::byte> buf;
  for (auto _ : state) {
    buf.clear();
    proto::write_frame(buf, proto::MsgType::Snapshot, 1, [&](proto::Writer& w) {
      proto::write_snapshot_header(w, 1, static_cast<std::uint32_t>(big().tree.size() - 1));
      big().tree.export_snapshot([&](const SnapshotEntry& e) { proto::write_snapshot_entry(w, e); });
    });
    benchmark::DoNotOptimize(buf.data());
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * buf.size()));
  state.SetItemsProcessed(state.iterations() * (big().tree.size() - 1));
  state.counters["bytes/entry"] = static_cast<double>(buf.size()) / static_cast<double>(big().tree.size() - 1);
}
BENCHMARK(BM_EncodeSnapshot100k)->Unit(benchmark::kMillisecond);

void BM_DecodeSnapshot100kStreaming(benchmark::State& state) {
  const std::vector<std::byte> buf = encoded_snapshot();
  for (auto _ : state) {
    proto::Reader r{std::span<const std::byte>(buf).subspan(proto::kHeaderSize)};
    std::uint64_t total = 0;
    auto gen = proto::read_snapshot(r, [&](const SnapshotEntry& e) { total += e.name.size() + e.attr.size; });
    benchmark::DoNotOptimize(gen);
    benchmark::DoNotOptimize(total);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * buf.size()));
  state.SetItemsProcessed(state.iterations() * (big().tree.size() - 1));
}
BENCHMARK(BM_DecodeSnapshot100kStreaming)->Unit(benchmark::kMillisecond);

void BM_DecodeSnapshot100kIntoTree(benchmark::State& state) {
  // End-to-end cost of receiving a snapshot: decode + build the metadata tree.
  const std::vector<std::byte> buf = encoded_snapshot();
  std::vector<SnapshotEntry> entries;
  entries.reserve(big().tree.size());
  for (auto _ : state) {
    entries.clear();
    proto::Reader r{std::span<const std::byte>(buf).subspan(proto::kHeaderSize)};
    (void)proto::read_snapshot(r, [&](const SnapshotEntry& e) { entries.push_back(e); });
    MetadataTree t;
    (void)t.load_snapshot(entries);
    benchmark::DoNotOptimize(t.size());
  }
  state.SetItemsProcessed(state.iterations() * (big().tree.size() - 1));
}
BENCHMARK(BM_DecodeSnapshot100kIntoTree)->Unit(benchmark::kMillisecond);

void BM_InvalidationBatchRoundTrip(benchmark::State& state) {
  proto::InvalidationBatch b;
  b.generation = 5;
  for (int i = 0; i < 256; ++i)
    b.ops.push_back({i % 4 == 0 ? InvalidationKind::Remove : InvalidationKind::Upsert,
                     "module_3/component2/Source_File_" + std::to_string(i) + ".cpp", kFileAttr});
  std::vector<std::byte> buf;
  for (auto _ : state) {
    buf.clear();
    proto::Writer w(buf);
    proto::write_invalidation(w, b);
    proto::Reader r(buf);
    auto got = proto::read_invalidation(r);
    benchmark::DoNotOptimize(got);
  }
  state.SetItemsProcessed(state.iterations() * 256);
}
BENCHMARK(BM_InvalidationBatchRoundTrip);

void BM_FrameHeaderRoundTrip(benchmark::State& state) {
  std::array<std::byte, proto::kHeaderSize> buf{};
  proto::FrameHeader h{.type = proto::MsgType::ReadRequest, .flags = 0, .payload_len = 100, .request_id = 7};
  for (auto _ : state) {
    proto::encode_header(h, buf);
    auto d = proto::decode_header(buf);
    benchmark::DoNotOptimize(d);
    h.request_id++;
  }
}
BENCHMARK(BM_FrameHeaderRoundTrip);

}  // namespace
}  // namespace wsld::bench
