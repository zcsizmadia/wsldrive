#pragma once

#include "core/error.hpp"
#include "core/flat_map.hpp"
#include "core/string_pool.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wsld {

enum class LookupMode : std::uint8_t {
  Exact,            // POSIX semantics
  CaseInsensitive,  // NTFS-facing semantics: exact match preferred, folded match otherwise
};

/// One entry of a wire/snapshot representation of a tree. `parent` is 0 for the
/// root and k for the k-th entry (1-based) of the same snapshot, so parents
/// always precede their children.
struct SnapshotEntry {
  std::uint32_t parent;
  std::string_view name;
  Attributes attr;
};

/// In-memory directory tree holding the complete metadata of a mounted root.
///
/// Design goals: every metadata operation the remote side needs (lookup, stat,
/// readdir, negative lookup) is served from RAM with zero heap allocation and
/// O(path components) work. Nodes live in one contiguous vector and are
/// addressed by index; names are interned; child lookup is a single probe into
/// an open-addressing hash table keyed by (parent, name id).
///
/// Not thread-safe; the owner serialises access (single writer, readers under a
/// shared lock, or a single-threaded event loop).
class MetadataTree {
 public:
  struct Node {
    NameId name = kInvalidName;  // kInvalidName marks a free slot
    NameId folded_name = kInvalidName;
    NodeId parent = kInvalidNode;
    NodeId first_child = kInvalidNode;
    NodeId last_child = kInvalidNode;
    NodeId next_sibling = kInvalidNode;
    NodeId prev_sibling = kInvalidNode;
    std::uint32_t child_count = 0;
    Attributes attr{};

    [[nodiscard]] bool is_dir() const noexcept { return attr.kind == NodeKind::Directory; }
  };

  struct Stats {
    std::size_t nodes;
    std::size_t names;
    std::size_t name_bytes;
    std::size_t case_collisions;  // sibling pairs that differ only by case
    // Snapshot entries the last load_snapshot could not represent (and the
    // descendants dropped with them) — e.g. a name containing a backslash,
    // which systemd creates (system-systemd\x2dcryptsetup.slice). Dropping
    // them keeps one odd name from costing the whole tree.
    std::size_t dropped;
  };

  MetadataTree();

  [[nodiscard]] NodeId root() const noexcept { return 0; }
  [[nodiscard]] const Node& node(NodeId id) const noexcept { return nodes_[id]; }
  [[nodiscard]] std::string_view name(NodeId id) const noexcept { return names_.view(nodes_[id].name); }
  [[nodiscard]] bool valid(NodeId id) const noexcept { return id < nodes_.size() && nodes_[id].name != kInvalidName; }
  [[nodiscard]] std::size_t size() const noexcept { return live_; }
  [[nodiscard]] Stats stats() const noexcept;

  // --- mutation --------------------------------------------------------------

  /// Adds a child. Fails with AlreadyExists if an exact-name sibling exists.
  Result<NodeId> insert(NodeId parent, std::string_view name, const Attributes& attr);
  /// Adds a child or refreshes the attributes of the existing exact-name child.
  Result<NodeId> upsert(NodeId parent, std::string_view name, const Attributes& attr);
  Result<void> set_attributes(NodeId id, const Attributes& attr);
  /// Removes a node and its whole subtree. The root cannot be removed.
  Result<void> remove(NodeId id);
  Result<void> rename(NodeId id, NodeId new_parent, std::string_view new_name);

  /// Path-based conveniences used when applying invalidations. Paths use '/' or
  /// '\\' separators; leading separators are ignored.
  Result<NodeId> ensure_directory_path(std::string_view path);
  Result<NodeId> upsert_path(std::string_view path, const Attributes& attr);
  /// Removes the node at `path`, resolved with `mode` - a client that looks
  /// paths up case-insensitively must remove them the same way or it leaves
  /// the node behind under its real spelling.
  Result<void> remove_path(std::string_view path, LookupMode mode = LookupMode::Exact);

  /// Replaces the whole tree with the given snapshot.
  Result<void> load_snapshot(std::span<const SnapshotEntry> entries);
  void clear();

  // --- lookup ----------------------------------------------------------------

  [[nodiscard]] std::optional<NodeId> find_child(NodeId parent, std::string_view name,
                                                 LookupMode mode = LookupMode::Exact) const noexcept;
  [[nodiscard]] std::optional<NodeId> lookup(std::string_view path, LookupMode mode = LookupMode::Exact) const noexcept;

  /// Visits the children of `dir` in insertion order. `f(NodeId)`.
  template <class F>
  void for_each_child(NodeId dir, F&& f) const {
    for (NodeId c = nodes_[dir].first_child; c != kInvalidNode; c = nodes_[c].next_sibling) f(c);
  }

  /// Reconstructs the path of a node relative to the root (no leading separator).
  [[nodiscard]] std::string path_of(NodeId id, char sep = '/') const;

  /// Emits the tree breadth-first as SnapshotEntry values. Parent indices follow
  /// the SnapshotEntry contract, so the output can be fed to load_snapshot.
  template <class F>
  void export_snapshot(F&& on_entry) const {
    std::vector<std::pair<NodeId, std::uint32_t>> queue;
    queue.reserve(live_);
    queue.emplace_back(root(), 0u);
    std::uint32_t next_index = 1;
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
      const auto [id, idx] = queue[qi];
      for (NodeId c = nodes_[id].first_child; c != kInvalidNode; c = nodes_[c].next_sibling) {
        const Node& n = nodes_[c];
        on_entry(SnapshotEntry{idx, names_.view(n.name), n.attr});
        if (n.is_dir()) queue.emplace_back(c, next_index);
        ++next_index;
      }
    }
  }

 private:
  static constexpr std::uint64_t key(NodeId parent, NameId name) noexcept {
    return (static_cast<std::uint64_t>(parent) << 32) | name;
  }
  static bool valid_name(std::string_view name) noexcept;

  NodeId alloc_node();
  void link_child(NodeId parent, NodeId child) noexcept;
  void unlink_child(NodeId child) noexcept;
  void index_node(NodeId id);
  void unindex_node(NodeId id);
  void release_subtree(NodeId id);

  StringPool names_;
  std::vector<Node> nodes_;
  std::vector<NodeId> free_;
  U64Map exact_;   // (parent, name)        -> node
  U64Map folded_;  // (parent, folded name) -> first node inserted with that folding
  std::size_t live_ = 0;
  std::size_t collisions_ = 0;
  std::size_t dropped_ = 0;  // entries the last load_snapshot could not represent
};

}  // namespace wsld
