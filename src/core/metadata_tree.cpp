#include "core/metadata_tree.hpp"

#include "core/path.hpp"
#include "core/unicode.hpp"

#include <algorithm>
#include <cassert>

namespace wsld {

namespace {

// Scratch buffer for case folding on lookups; avoids a heap allocation per call
// after warm-up. Lookups are const and may run on several threads.
std::string& fold_scratch() {
  thread_local std::string buf;
  buf.clear();
  return buf;
}

constexpr Attributes kImplicitDirectory{.size = 0, .mtime_ns = 0, .mode = 0755, .kind = NodeKind::Directory};

}  // namespace

MetadataTree::MetadataTree() {
  nodes_.reserve(1024);
  Node root;
  root.name = names_.intern("");
  root.folded_name = root.name;
  root.attr = kImplicitDirectory;
  nodes_.push_back(root);
  live_ = 1;
}

MetadataTree::Stats MetadataTree::stats() const noexcept {
  return Stats{live_, names_.size(), names_.bytes(), collisions_, dropped_};
}

bool MetadataTree::valid_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char c : name)
    if (is_path_separator(c) || c == '\0') return false;
  return true;
}

NodeId MetadataTree::alloc_node() {
  if (!free_.empty()) {
    const NodeId id = free_.back();
    free_.pop_back();
    nodes_[id] = Node{};
    return id;
  }
  nodes_.emplace_back();
  return static_cast<NodeId>(nodes_.size() - 1);
}

void MetadataTree::link_child(NodeId parent, NodeId child) noexcept {
  Node& p = nodes_[parent];
  Node& c = nodes_[child];
  c.parent = parent;
  c.next_sibling = kInvalidNode;
  c.prev_sibling = p.last_child;
  if (p.last_child != kInvalidNode)
    nodes_[p.last_child].next_sibling = child;
  else
    p.first_child = child;
  p.last_child = child;
  ++p.child_count;
}

void MetadataTree::unlink_child(NodeId child) noexcept {
  Node& c = nodes_[child];
  Node& p = nodes_[c.parent];
  if (c.prev_sibling != kInvalidNode)
    nodes_[c.prev_sibling].next_sibling = c.next_sibling;
  else
    p.first_child = c.next_sibling;
  if (c.next_sibling != kInvalidNode)
    nodes_[c.next_sibling].prev_sibling = c.prev_sibling;
  else
    p.last_child = c.prev_sibling;
  --p.child_count;
  c.prev_sibling = c.next_sibling = kInvalidNode;
}

void MetadataTree::index_node(NodeId id) {
  const Node& n = nodes_[id];
  exact_.insert_or_assign(key(n.parent, n.name), id);
  if (!folded_.insert_if_absent(key(n.parent, n.folded_name), id)) ++collisions_;
}

void MetadataTree::unindex_node(NodeId id) {
  const Node& n = nodes_[id];
  exact_.erase(key(n.parent, n.name));
  const std::uint64_t fk = key(n.parent, n.folded_name);
  const std::uint32_t* owner = folded_.find(fk);
  if (owner == nullptr) return;
  if (*owner != id) {
    // A case-colliding sibling owns the folded slot; we were the shadowed one.
    --collisions_;
    return;
  }
  folded_.erase(fk);
  if (collisions_ == 0) return;
  // Hand the folded slot to a remaining sibling with the same folding, if any.
  for (NodeId s = nodes_[n.parent].first_child; s != kInvalidNode; s = nodes_[s].next_sibling) {
    if (s != id && nodes_[s].folded_name == n.folded_name) {
      folded_.insert_if_absent(fk, s);
      --collisions_;
      break;
    }
  }
}

Result<NodeId> MetadataTree::insert(NodeId parent, std::string_view name, const Attributes& attr) {
  if (!valid(parent)) return fail(Errc::NotFound);
  if (!nodes_[parent].is_dir()) return fail(Errc::NotADirectory);
  if (!valid_name(name)) return fail(Errc::InvalidPath);

  const NameId nid = names_.intern(name);
  if (exact_.contains(key(parent, nid))) return fail(Errc::AlreadyExists);

  NameId fid = nid;
  if (!is_ascii(name) || std::any_of(name.begin(), name.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
    std::string& folded = fold_scratch();
    casefold_append(name, folded);
    fid = names_.intern(folded);
  }

  const NodeId id = alloc_node();
  Node& n = nodes_[id];
  n.name = nid;
  n.folded_name = fid;
  n.attr = attr;
  link_child(parent, id);
  index_node(id);
  ++live_;
  return id;
}

Result<NodeId> MetadataTree::upsert(NodeId parent, std::string_view name, const Attributes& attr) {
  if (!valid(parent)) return fail(Errc::NotFound);
  if (const auto existing = find_child(parent, name, LookupMode::Exact)) {
    if (auto r = set_attributes(*existing, attr); !r) return fail(r.error());
    return *existing;
  }
  return insert(parent, name, attr);
}

Result<void> MetadataTree::set_attributes(NodeId id, const Attributes& attr) {
  if (!valid(id)) return fail(Errc::NotFound);
  Node& n = nodes_[id];
  if (n.is_dir() && attr.kind != NodeKind::Directory && n.child_count != 0) {
    // A directory turned into a file: whatever we knew about its children is stale.
    while (n.first_child != kInvalidNode) {
      const NodeId c = n.first_child;
      unlink_child(c);
      release_subtree(c);
    }
  }
  n.attr = attr;
  return {};
}

void MetadataTree::release_subtree(NodeId id) {
  // Iterative post-order release. The caller has already unlinked `id` from its
  // parent; descendants are unlinked one by one here so that unindex_node can
  // still see their remaining siblings (needed for case-collision bookkeeping).
  std::vector<NodeId> stack{id};
  while (!stack.empty()) {
    const NodeId cur = stack.back();
    if (nodes_[cur].first_child != kInvalidNode) {
      for (NodeId c = nodes_[cur].first_child; c != kInvalidNode; c = nodes_[c].next_sibling) stack.push_back(c);
      continue;  // revisit `cur` once its children are gone
    }
    stack.pop_back();
    if (cur != id) unlink_child(cur);
    unindex_node(cur);
    nodes_[cur].name = kInvalidName;
    free_.push_back(cur);
    --live_;
  }
}

Result<void> MetadataTree::remove(NodeId id) {
  if (!valid(id)) return fail(Errc::NotFound);
  if (id == root()) return fail(Errc::InvalidArgument);
  unlink_child(id);
  release_subtree(id);
  return {};
}

Result<void> MetadataTree::rename(NodeId id, NodeId new_parent, std::string_view new_name) {
  if (!valid(id) || !valid(new_parent)) return fail(Errc::NotFound);
  if (id == root()) return fail(Errc::InvalidArgument);
  if (!nodes_[new_parent].is_dir()) return fail(Errc::NotADirectory);
  if (!valid_name(new_name)) return fail(Errc::InvalidPath);
  // Refuse to move a directory into its own subtree.
  for (NodeId a = new_parent; a != kInvalidNode; a = nodes_[a].parent)
    if (a == id) return fail(Errc::InvalidArgument);

  const NameId nid = names_.intern(new_name);
  if (const std::uint32_t* existing = exact_.find(key(new_parent, nid)); existing != nullptr) {
    if (*existing == id) return {};
    return fail(Errc::AlreadyExists);
  }
  NameId fid = nid;
  if (!is_ascii(new_name) || std::any_of(new_name.begin(), new_name.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
    std::string& folded = fold_scratch();
    casefold_append(new_name, folded);
    fid = names_.intern(folded);
  }

  unindex_node(id);
  unlink_child(id);
  Node& n = nodes_[id];
  n.name = nid;
  n.folded_name = fid;
  link_child(new_parent, id);
  index_node(id);
  return {};
}

std::optional<NodeId> MetadataTree::find_child(NodeId parent, std::string_view name, LookupMode mode) const noexcept {
  if (!valid(parent) || !nodes_[parent].is_dir()) return std::nullopt;
  if (const auto nid = names_.find(name)) {
    if (const std::uint32_t* v = exact_.find(key(parent, *nid)); v != nullptr) return *v;
  }
  if (mode == LookupMode::Exact) return std::nullopt;
  std::string& folded = fold_scratch();
  casefold_append(name, folded);
  const auto fid = names_.find(folded);
  if (!fid) return std::nullopt;
  if (const std::uint32_t* v = folded_.find(key(parent, *fid)); v != nullptr) return *v;
  return std::nullopt;
}

std::optional<NodeId> MetadataTree::lookup(std::string_view path, LookupMode mode) const noexcept {
  NodeId cur = root();
  std::size_t i = 0;
  const std::size_t n = path.size();
  while (i < n) {
    while (i < n && is_path_separator(path[i])) ++i;
    std::size_t j = i;
    while (j < n && !is_path_separator(path[j])) ++j;
    const std::string_view comp = path.substr(i, j - i);
    i = j;
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") {
      if (nodes_[cur].parent != kInvalidNode) cur = nodes_[cur].parent;
      continue;
    }
    const auto next = find_child(cur, comp, mode);
    if (!next) return std::nullopt;
    cur = *next;
  }
  return cur;
}

Result<NodeId> MetadataTree::ensure_directory_path(std::string_view path) {
  NodeId cur = root();
  std::size_t i = 0;
  const std::size_t n = path.size();
  while (i < n) {
    while (i < n && is_path_separator(path[i])) ++i;
    std::size_t j = i;
    while (j < n && !is_path_separator(path[j])) ++j;
    const std::string_view comp = path.substr(i, j - i);
    i = j;
    if (comp.empty() || comp == ".") continue;
    if (comp == "..") return fail(Errc::InvalidPath);
    if (const auto next = find_child(cur, comp, LookupMode::Exact)) {
      if (!nodes_[*next].is_dir()) return fail(Errc::NotADirectory);
      cur = *next;
      continue;
    }
    auto created = insert(cur, comp, kImplicitDirectory);
    if (!created) return created;
    cur = *created;
  }
  return cur;
}

Result<NodeId> MetadataTree::upsert_path(std::string_view path, const Attributes& attr) {
  std::string norm = normalize_path(path);
  if (norm.empty()) return fail(Errc::InvalidPath);
  const auto [parent, leaf] = split_parent(norm);
  auto dir = ensure_directory_path(parent);
  if (!dir) return dir;
  return upsert(*dir, leaf, attr);
}

Result<void> MetadataTree::remove_path(std::string_view path, LookupMode mode) {
  const auto id = lookup(path, mode);
  if (!id) return fail(Errc::NotFound);
  return remove(*id);
}

Result<void> MetadataTree::load_snapshot(std::span<const SnapshotEntry> entries) {
  clear();
  std::vector<NodeId> ids;
  ids.reserve(entries.size() + 1);
  ids.push_back(root());
  nodes_.reserve(entries.size() + 1);
  exact_.reserve(entries.size() + 1);
  folded_.reserve(entries.size() + 1);
  for (std::size_t k = 0; k < entries.size(); ++k) {
    const SnapshotEntry& e = entries[k];
    if (e.parent > k) return fail(Errc::Corrupt);  // parent must precede child
    // One entry we cannot represent must not cost the whole tree: drop it (and,
    // via the invalid placeholder, everything beneath it) and keep going. Real
    // trees contain such names — systemd escapes unit files as
    // `system-systemd\x2dcryptsetup.slice`, and a backslash is a path separator
    // here. Indices stay aligned because every entry still pushes an id.
    const NodeId parent = ids[e.parent];
    if (parent == kInvalidNode) {  // an ancestor was dropped
      ids.push_back(kInvalidNode);
      ++dropped_;
      continue;
    }
    auto id = insert(parent, e.name, e.attr);
    if (!id) {
      ids.push_back(kInvalidNode);
      ++dropped_;
      continue;
    }
    ids.push_back(*id);
  }
  return {};
}

void MetadataTree::clear() {
  names_.clear();
  nodes_.clear();
  free_.clear();
  exact_.clear();
  folded_.clear();
  collisions_ = 0;
  dropped_ = 0;
  Node root;
  root.name = names_.intern("");
  root.folded_name = root.name;
  root.attr = kImplicitDirectory;
  nodes_.push_back(root);
  live_ = 1;
}

std::string MetadataTree::path_of(NodeId id, char sep) const {
  if (!valid(id) || id == root()) return {};
  std::vector<NodeId> chain;
  for (NodeId cur = id; cur != root(); cur = nodes_[cur].parent) chain.push_back(cur);
  std::string out;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (!out.empty()) out.push_back(sep);
    out.append(name(*it));
  }
  return out;
}

}  // namespace wsld
