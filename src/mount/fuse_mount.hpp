#pragma once

#include "agent/client.hpp"
#include "core/error.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace wsld::mount {

/// Mounts a RemoteRoot as a filesystem via the FUSE3 API (WinFsp on Windows,
/// libfuse3 on Linux). Read-only for the Phase 1 MVP: metadata is served from
/// the client's in-RAM mirror (zero round-trips) and file contents are fetched
/// over the socket on demand.
///
/// The FUSE event loop runs on its own thread; `unmount()` stops it and joins.
class FuseMount {
 public:
  explicit FuseMount(agent::RemoteRoot& root) noexcept : root_(root) {}
  ~FuseMount();
  FuseMount(const FuseMount&) = delete;
  FuseMount& operator=(const FuseMount&) = delete;

  /// Mounts at `mountpoint` (a drive letter like "Z:" or a directory path) and
  /// starts serving. Returns once the volume is up. With `writeback`, writes to
  /// a file are buffered and coalesced, flushed on fsync/flush/release — fewer
  /// round-trips, at the cost of durability only at flush/close (opt-in).
  [[nodiscard]] Result<void> mount(const std::string& mountpoint, bool writeback = false);

  /// Signals the FUSE loop to exit, unmounts, and joins the loop thread.
  void unmount();

  [[nodiscard]] bool mounted() const noexcept { return mounted_.load(); }

 private:
  agent::RemoteRoot& root_;
  void* fuse_ = nullptr;  // struct fuse*
  std::string mountpoint_;
  std::thread loop_;
  std::atomic<bool> mounted_{false};
};

}  // namespace wsld::mount
