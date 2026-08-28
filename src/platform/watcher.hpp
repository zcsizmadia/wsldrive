#pragma once

#include "core/coalescer.hpp"
#include "core/error.hpp"

#include <filesystem>
#include <functional>
#include <memory>

namespace wsld::platform {

/// Delivers raw filesystem events for a directory tree. Events are reported on
/// an internal thread with paths normalised to '/'-separated form relative to
/// the watched root. Implementations: ReadDirectoryChangesW + IOCP on Windows,
/// fanotify/inotify on Linux.
class Watcher {
 public:
  virtual ~Watcher() = default;
  /// Stops delivery and joins the internal thread. Idempotent.
  virtual void stop() = 0;
};

using WatchCallback = std::function<void(const FsEvent&)>;

/// Creates and starts a watcher for `root`. Returns Unsupported where no
/// implementation exists for the current platform.
[[nodiscard]] Result<std::unique_ptr<Watcher>> make_watcher(const std::filesystem::path& root, WatchCallback cb);

}  // namespace wsld::platform
