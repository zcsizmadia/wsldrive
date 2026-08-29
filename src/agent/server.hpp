#pragma once

#include "core/coalescer.hpp"
#include "core/error.hpp"
#include "core/ignore.hpp"
#include "net/frame_channel.hpp"
#include "platform/watcher.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace wsld::agent {

/// Serves one directory tree to any number of connected peers: answers
/// snapshot and read requests, and pushes coalesced invalidations produced by
/// the platform watcher.
class RootServer {
 public:
  struct Options {
    std::filesystem::path root;
    bool watch = true;
    Coalescer::Options coalescer{};
    // Snapshot replies are split into frames of about this many payload bytes so
    // arbitrarily large trees are not bound by the single-frame limit.
    std::size_t snapshot_chunk_bytes = 4u << 20;
    // Stay on the root's filesystem: mount points (pseudo-filesystems like
    // /proc and /sys, and foreign mounts like /mnt/c) are reported but not
    // descended into. Turn off only to deliberately serve across mounts.
    bool one_file_system = true;
    // Shared secret a peer must present in its Hello. Empty disables the check
    // (only when the operator passes --insecure-no-auth).
    std::string token;
  };

  explicit RootServer(Options opts);
  ~RootServer();
  RootServer(const RootServer&) = delete;
  RootServer& operator=(const RootServer&) = delete;

  /// Starts the watcher/flusher threads. Without a watcher (Unsupported) the
  /// server still works, only without live invalidations.
  [[nodiscard]] Result<void> start();
  void stop();

  /// Handles frames on `ch` until the peer disconnects. Blocking; call from a
  /// per-connection thread. The channel must outlive the call.
  void serve(net::FrameChannel& ch);

  [[nodiscard]] bool watching() const noexcept { return watcher_ != nullptr; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_.load(); }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return opts_.root; }

 private:
  Result<void> handle(const net::Frame& f, net::FrameChannel& ch);
  Result<void> send_snapshot(std::uint64_t request_id, net::FrameChannel& ch);
  Result<void> send_read(const net::Frame& f, net::FrameChannel& ch);
  Result<void> send_read_many(const net::Frame& f, net::FrameChannel& ch);
  Result<void> handle_mutation(const net::Frame& f, net::FrameChannel& ch);
  Result<void> send_error(std::uint64_t request_id, Errc code, std::string_view detail, net::FrameChannel& ch);

  // Resolves a client-supplied relative path against the root, rejecting escapes.
  [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view rel) const;

  void on_event(const FsEvent& ev);
  void flush_loop();
  void broadcast(const std::vector<std::byte>& frame);

  Options opts_;
  std::atomic<std::uint64_t> generation_{1};

  IgnoreRules ignore_;
  std::unique_ptr<platform::Watcher> watcher_;
  std::mutex coalescer_mu_;
  std::condition_variable coalescer_cv_;
  Coalescer coalescer_;
  std::thread flusher_;
  std::atomic<bool> stopping_{false};

  std::mutex peers_mu_;
  std::vector<net::FrameChannel*> peers_;
  // broadcast() sends outside peers_mu_ so a slow peer cannot stall everyone;
  // this counter lets a departing session wait until no send still holds its
  // channel pointer.
  std::condition_variable peers_cv_;
  std::size_t broadcasts_in_flight_ = 0;
};

}  // namespace wsld::agent
