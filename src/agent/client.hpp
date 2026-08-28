#pragma once

#include "core/error.hpp"
#include "core/metadata_tree.hpp"
#include "net/frame_channel.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wsld::agent {

/// Client side of a served root: keeps a MetadataTree mirror up to date from
/// snapshots and pushed invalidations, and issues reads.
///
/// A reader thread owns `receive()`; responses are matched to waiting callers
/// by request id, invalidations are applied to the tree under the write lock.
class RemoteRoot {
 public:
  struct Stats {
    std::uint64_t invalidation_batches = 0;
    std::uint64_t invalidation_ops = 0;
    std::uint64_t generation = 0;
    std::size_t snapshot_bytes = 0;
    std::chrono::nanoseconds last_snapshot_time{0};
    std::uint64_t read_cache_hits = 0;
    std::uint64_t read_cache_misses = 0;
    std::uint64_t read_cache_bytes = 0;
  };

  using InvalidationHook = std::function<void(const proto::InvalidationBatch&)>;

  explicit RemoteRoot(std::unique_ptr<net::FrameChannel> ch);
  ~RemoteRoot();
  RemoteRoot(const RemoteRoot&) = delete;
  RemoteRoot& operator=(const RemoteRoot&) = delete;

  /// Starts the reader thread and performs the Hello handshake.
  [[nodiscard]] Result<proto::Hello> connect(std::chrono::milliseconds timeout = std::chrono::seconds(10));

  /// Requests a full snapshot and replaces the tree with it.
  [[nodiscard]] Result<void> fetch_snapshot(std::chrono::milliseconds timeout = std::chrono::minutes(5));

  /// Reads `length` bytes at `offset` of `path`; returns fewer at EOF. Small
  /// files are cached in RAM and served locally on repeat reads until an
  /// invalidation (or a size/mtime change) supersedes them.
  [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Largest file the read cache will hold whole; larger reads stream uncached.
  static constexpr std::uint64_t kMaxCacheableFile = 8u << 20;

  [[nodiscard]] Result<std::chrono::nanoseconds> ping(std::chrono::milliseconds timeout = std::chrono::seconds(5));

  // --- write-through mutations (Phase 3) -------------------------------------
  // Each performs the operation on the far side, then optimistically updates the
  // local mirror so an immediate stat/read is consistent; the server's watcher
  // also pushes an invalidation that reconciles any drift.
  [[nodiscard]] Result<void> create_file(std::string_view path, std::uint32_t mode = 0644,
                                         std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<std::uint64_t> write(std::string_view path, std::uint64_t offset,
                                            std::span<const std::byte> data,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> truncate(std::string_view path, std::uint64_t size,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> mkdir(std::string_view path, std::uint32_t mode = 0755,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> unlink(std::string_view path,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> rmdir(std::string_view path,
                                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
  [[nodiscard]] Result<void> rename(std::string_view from, std::string_view to,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Read access to the mirrored tree. Hold the lock only briefly.
  template <class F>
  auto with_tree(F&& f) const -> decltype(f(std::declval<const MetadataTree&>())) {
    std::shared_lock lock(tree_mu_);
    return f(tree_);
  }

  void set_invalidation_hook(InvalidationHook hook);
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] bool connected() const noexcept { return !closed_.load(); }
  void close();

 private:
  struct Pending {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    proto::MsgType type{};
    std::vector<std::byte> payload;
    // Streaming (multi-frame) reply accumulation, used for snapshots.
    bool streaming = false;
    bool stream_ok = true;
    std::uint64_t snap_generation = 0;
    std::size_t snap_bytes = 0;
    std::vector<std::string> snap_names;      // owns entry names across frames
    std::vector<SnapshotEntry> snap_entries;  // .name filled in by the caller at the end
  };

  [[nodiscard]] Result<std::shared_ptr<Pending>> request(proto::MsgType type, std::span<const std::byte> payload,
                                                         std::chrono::milliseconds timeout, bool streaming = false);
  [[nodiscard]] Result<std::vector<std::byte>> read_remote(std::string_view path, std::uint64_t offset,
                                                           std::uint32_t length, std::chrono::milliseconds timeout);
  void reader_loop();
  void apply_invalidation(std::span<const std::byte> payload);
  void fail_all_pending();
  void drop_cached(std::string_view path);  // evict one read-cache entry

  std::unique_ptr<net::FrameChannel> ch_;
  std::thread reader_;
  std::atomic<bool> closed_{false};  // connection is dead (set by reader on EOF or by close())
  std::once_flag cleanup_;           // ensures shutdown + reader join happen exactly once
  std::atomic<std::uint64_t> next_request_{1};

  std::mutex pending_mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Pending>> pending_;

  mutable std::shared_mutex tree_mu_;
  MetadataTree tree_;

  mutable std::mutex stats_mu_;
  Stats stats_;
  InvalidationHook hook_;

  // In-RAM read cache: path -> whole-file contents, validated against the tree's
  // (mtime, size) and dropped on invalidation. LRU-evicted to a byte budget.
  struct CacheEntry {
    std::int64_t mtime_ns = 0;
    std::uint64_t size = 0;
    std::uint64_t last_use = 0;
    std::vector<std::byte> data;
  };
  mutable std::mutex rcache_mu_;
  std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>> rcache_;
  std::uint64_t rcache_bytes_ = 0;
  std::uint64_t rcache_tick_ = 0;
  std::uint64_t rcache_cap_ = 256u << 20;  // 256 MiB
};

}  // namespace wsld::agent
