#pragma once

#include "core/hash_util.hpp"
#include "core/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wsld {

/// Raw filesystem event as reported by a platform watcher, already normalised
/// to a '/'-separated path relative to the watched root.
enum class FsEventKind : std::uint8_t {
  Created,
  Modified,
  Removed,
  RenamedFrom,  // old name of a rename
  RenamedTo,    // new name of a rename
  Overflow,     // the watcher's buffer overflowed; events were lost
};

struct FsEvent {
  FsEventKind kind;
  std::string_view path;
};

/// A planned invalidation. Attributes are not resolved here: the watcher side
/// stats the path when the batch is sent, so a burst of writes costs one stat.
struct PlannedOp {
  InvalidationKind kind;
  std::string path;
  // The entry is new at this path (a Created or RenamedTo was seen, and no
  // removal since). A directory that appears by rename brings a whole subtree
  // the watcher never reports, so the sender enumerates it; a Modified on a
  // directory the peers already know must not trigger that scan.
  bool appeared = false;

  friend bool operator==(const PlannedOp& a, const PlannedOp& b) noexcept {
    return a.kind == b.kind && a.path == b.path;  // `appeared` is advisory
  }
};

/// Collapses bursts of watcher events into a minimal ordered batch.
///
/// - Repeated events on one path collapse to the last relevant operation.
/// - Removing a directory drops pending operations on everything below it.
/// - An overflow discards everything and yields a single Rescan of the root.
/// - A batch becomes ready after a quiet period, after a maximum latency since
///   its first event, or when the pending count reaches a cap.
///
/// Time is injected so the class is deterministic and testable.
class Coalescer {
 public:
  using clock = std::chrono::steady_clock;

  struct Options {
    std::size_t max_pending = 4096;
    clock::duration quiet_period = std::chrono::milliseconds(2);
    clock::duration max_latency = std::chrono::milliseconds(25);
  };

  Coalescer() noexcept : Coalescer(Options{}) {}
  explicit Coalescer(Options opts) noexcept : opts_(opts) {}

  void push(const FsEvent& ev, clock::time_point now);

  [[nodiscard]] bool ready(clock::time_point now) const noexcept;

  /// When `ready` would flip to true if no further events arrive; nullopt if idle.
  [[nodiscard]] std::optional<clock::time_point> deadline() const noexcept;

  /// Returns the pending batch ordered by the arrival time of each path's last
  /// event, and resets the coalescer.
  [[nodiscard]] std::vector<PlannedOp> take();

  [[nodiscard]] std::size_t pending() const noexcept { return overflow_ ? 1 : pending_.size(); }
  [[nodiscard]] bool empty() const noexcept { return !overflow_ && pending_.empty(); }
  [[nodiscard]] bool overflowed() const noexcept { return overflow_; }

 private:
  struct Entry {
    InvalidationKind kind;
    std::uint64_t seq;
    bool appeared;
  };

  Options opts_;
  std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> pending_;
  std::uint64_t seq_ = 0;
  bool overflow_ = false;
  clock::time_point first_{};
  clock::time_point last_{};
};

}  // namespace wsld
