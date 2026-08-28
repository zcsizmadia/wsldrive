// Linux watcher placeholder. The fanotify (FAN_MARK_FILESYSTEM) implementation
// with inotify fallback is the next Linux milestone; until then the agent still
// serves snapshots and reads, just without live invalidations.
#include "platform/watcher.hpp"

namespace wsld::platform {

Result<std::unique_ptr<Watcher>> make_watcher(const std::filesystem::path&, WatchCallback) {
  return fail(Errc::Unsupported);
}

}  // namespace wsld::platform
