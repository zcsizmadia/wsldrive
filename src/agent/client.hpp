#pragma once

#include "core/error.hpp"
#include "core/metadata_tree.hpp"
#include "net/frame_channel.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // Cold-read instrumentation: time spent fetching bytes on a read miss (the
    // boundary cost that warming hides), and what the background prefetcher did.
    std::uint64_t read_miss_fetch_ns = 0;
    std::uint64_t prefetch_files = 0;
    std::uint64_t prefetch_bytes = 0;
  };

  using InvalidationHook = std::function<void(const proto::InvalidationBatch&)>;

  explicit RemoteRoot(std::unique_ptr<net::FrameChannel> ch);
  ~RemoteRoot();
  RemoteRoot(const RemoteRoot&) = delete;
  RemoteRoot& operator=(const RemoteRoot&) = delete;

  /// Shared secret presented in the Hello handshake; must match the agent's.
  /// Set before connect().
  void set_auth_token(std::string token) { token_ = std::move(token); }

  /// Starts the reader thread and performs the Hello handshake.
  [[nodiscard]] Result<proto::Hello> connect(std::chrono::milliseconds timeout = std::chrono::seconds(10));

  /// Requests a full snapshot and replaces the tree with it.
  [[nodiscard]] Result<void> fetch_snapshot(std::chrono::milliseconds timeout = std::chrono::minutes(5));

  /// Reads `length` bytes at `offset` of `path`; returns fewer at EOF. Small
  /// files are cached in RAM and served locally on repeat reads until an
  /// invalidation (or a size/mtime change) supersedes them.
  [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Reads into a caller-supplied buffer and returns the byte count. On a cache
  /// hit this copies straight from the cached file into `out`, so the hot path
  /// (a mount reading a cached file in chunks) does no intermediate allocation.
  /// Misses take the same path as read().
  [[nodiscard]] Result<std::size_t> read_into(std::string_view path, std::uint64_t offset, std::span<std::byte> out,
                                              std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /// Largest file the read cache will hold whole; larger reads stream uncached.
  static constexpr std::uint64_t kMaxCacheableFile = 8u << 20;

  /// Reads several files in one round-trip; each result is the whole file, or
  /// nullopt if the server could not return it (caller may fetch individually).
  [[nodiscard]] Result<std::vector<std::optional<std::vector<std::byte>>>> read_many(
      const std::vector<std::string>& paths, std::chrono::milliseconds timeout = std::chrono::seconds(30));

  [[nodiscard]] Result<std::chrono::nanoseconds> ping(std::chrono::milliseconds timeout = std::chrono::seconds(5));

  /// Proactively warm the read cache after mount: queue every directory whose
  /// small files fit within the cache budget for background prefetch, so the
  /// first reads a tool makes are already warm (cold reads avoided). Returns the
  /// number of directories queued; skips directories once the estimated small-
  /// file bytes would exceed the cache cap (huge trees fall back to lazy
  /// read-ahead). Non-blocking — the background prefetcher does the work.
  std::size_t warm_cache();

  /// Blocks until the background prefetcher has drained its queue (or the
  /// connection closes / the timeout elapses). Mainly for tests and benchmarks.
  void wait_prefetch_idle(std::chrono::milliseconds timeout = std::chrono::minutes(5));

  /// Byte budget for the in-RAM read cache (default 256 MiB). Lowering it evicts
  /// least-recently-used entries immediately.
  void set_read_cache_limit(std::uint64_t bytes);

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
  void cache_put(std::string path, std::int64_t mtime_ns, std::uint64_t size, std::vector<std::byte> data);

  // Background read-ahead: on a read miss, the file's directory is queued and a
  // worker bulk-fetches its not-yet-cached siblings so later reads hit the cache.
  void enqueue_prefetch(std::string dir);
  void prefetch_loop();

  std::string token_;  // presented in Hello
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
    // Position of this entry's key in `lru_` (front = most recently used), so a
    // hit reorders and an eviction picks a victim in O(1).
    std::list<std::string>::iterator lru{};
    // Shared so a reader can take a reference under the lock and copy its slice
    // after releasing it — the cache never mutates a buffer once published.
    std::shared_ptr<const std::vector<std::byte>> data;
  };
  mutable std::mutex rcache_mu_;
  std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>> rcache_;
  std::list<std::string> lru_;  // most-recently-used first
  std::uint64_t rcache_bytes_ = 0;
  std::uint64_t rcache_cap_ = 256u << 20;  // 256 MiB

  // Erases one entry, keeping `lru_` and the byte count in step. Call under
  // rcache_mu_.
  void cache_erase(std::unordered_map<std::string, CacheEntry, StringHash, std::equal_to<>>::iterator it);
  // Drops least-recently-used entries until the budget is met. Call under rcache_mu_.
  void evict_locked();

  std::thread prefetch_;
  std::mutex pf_mu_;
  std::condition_variable pf_cv_;
  std::condition_variable pf_done_cv_;       // signalled when the queue drains to empty
  std::deque<std::string> pf_queue_;
  std::unordered_set<std::string> pf_seen_;  // directories already queued/prefetched
  std::atomic<bool> pf_stop_{false};
  std::atomic<std::size_t> pf_pending_{0};   // dirs queued but not yet fully processed
  static constexpr std::size_t kPrefetchBatchBytes = 4u << 20;
  static constexpr std::size_t kPrefetchBatchCount = 512;
};

}  // namespace wsld::agent
