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

  /// Reads `length` bytes at `offset` of `path`; returns fewer at EOF.
  [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view path, std::uint64_t offset, std::uint32_t length,
                                                    std::chrono::milliseconds timeout = std::chrono::seconds(30));

  [[nodiscard]] Result<std::chrono::nanoseconds> ping(std::chrono::milliseconds timeout = std::chrono::seconds(5));

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
  };

  [[nodiscard]] Result<std::shared_ptr<Pending>> request(proto::MsgType type, std::span<const std::byte> payload,
                                                         std::chrono::milliseconds timeout);
  void reader_loop();
  void apply_invalidation(std::span<const std::byte> payload);
  void fail_all_pending();

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
};

}  // namespace wsld::agent
