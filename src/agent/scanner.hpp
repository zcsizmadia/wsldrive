#pragma once

#include "core/error.hpp"
#include "core/metadata_tree.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace wsld::agent {

struct ScanStats {
  std::size_t files = 0;
  std::size_t directories = 0;
  std::size_t symlinks = 0;
  std::size_t skipped = 0;  // sockets, fifos, devices, unreadable entries
};

/// Reads attributes for one filesystem entry. `follow` is false: symlinks are
/// reported as symlinks, not as their targets.
[[nodiscard]] Result<Attributes> read_attributes(const std::filesystem::path& p) noexcept;

/// Called with each entry's normalised relative path and directory flag; return
/// true to exclude it (and, for a directory, everything beneath it).
using SkipPredicate = std::function<bool(std::string_view rel, bool is_dir)>;

/// Identifies the filesystem a path lives on (st_dev on POSIX, the volume serial
/// on Windows). Returns nullopt when it cannot be determined.
[[nodiscard]] std::optional<std::uint64_t> device_id(const std::filesystem::path& p) noexcept;

/// Walks `root` breadth-first and emits SnapshotEntry values in the order and
/// with the parent indices that MetadataTree::load_snapshot expects.
/// Entry names are UTF-8. `on_entry` may retain nothing: the name view is only
/// valid for the duration of the call. If `skip` is set, matching entries are
/// omitted (directories are not descended into).
///
/// With `one_file_system` (the default), the walk stays on the filesystem `root`
/// lives on: a directory that is a mount point for another filesystem is still
/// reported, but is not descended into. That keeps pseudo-filesystems (`/proc`,
/// `/sys`, `/dev`, `/run`) and foreign mounts (notably `/mnt/c`, which would
/// otherwise pull the whole Windows drive back across the boundary) out of the
/// tree, which is what makes serving `/` practical.
[[nodiscard]] Result<ScanStats> scan_tree(const std::filesystem::path& root,
                                          const std::function<void(const SnapshotEntry&)>& on_entry,
                                          const SkipPredicate& skip = {}, bool one_file_system = true);

/// Converts a filesystem path component to UTF-8 (identity on POSIX).
[[nodiscard]] std::string filename_utf8(const std::filesystem::path& p);

/// Joins a root with a normalised relative protocol path ('/'-separated).
[[nodiscard]] std::filesystem::path join_relative(const std::filesystem::path& root, std::string_view rel);

}  // namespace wsld::agent
