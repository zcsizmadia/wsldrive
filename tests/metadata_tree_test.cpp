#include "core/metadata_tree.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace wsld {
namespace {

constexpr Attributes kDir{.size = 0, .mtime_ns = 1, .mode = 0755, .kind = NodeKind::Directory};
constexpr Attributes kFile{.size = 42, .mtime_ns = 2, .mode = 0644, .kind = NodeKind::File};

std::vector<std::string> child_names(const MetadataTree& t, NodeId dir) {
  std::vector<std::string> out;
  t.for_each_child(dir, [&](NodeId c) { out.emplace_back(t.name(c)); });
  return out;
}

TEST(MetadataTree, RootExists) {
  MetadataTree t;
  EXPECT_TRUE(t.valid(t.root()));
  EXPECT_TRUE(t.node(t.root()).is_dir());
  EXPECT_EQ(t.size(), 1u);
  EXPECT_EQ(t.lookup(""), t.root());
  EXPECT_EQ(t.lookup("/"), t.root());
  EXPECT_EQ(t.lookup("."), t.root());
  EXPECT_EQ(t.path_of(t.root()), "");
}

TEST(MetadataTree, InsertLookupAndPaths) {
  MetadataTree t;
  const NodeId src = *t.insert(t.root(), "src", kDir);
  const NodeId main = *t.insert(src, "main.cpp", kFile);
  EXPECT_EQ(t.size(), 3u);
  EXPECT_EQ(t.lookup("src/main.cpp"), main);
  EXPECT_EQ(t.lookup("/src//main.cpp"), main);
  EXPECT_EQ(t.lookup("src\\main.cpp"), main);
  EXPECT_EQ(t.lookup("src/./main.cpp"), main);
  EXPECT_EQ(t.lookup("src/../src/main.cpp"), main);
  EXPECT_EQ(t.lookup("../src"), src);  // ".." at the root stays at the root
  EXPECT_FALSE(t.lookup("src/missing.cpp").has_value());
  EXPECT_FALSE(t.lookup("src/main.cpp/x").has_value());  // file is not a directory
  EXPECT_EQ(t.path_of(main), "src/main.cpp");
  EXPECT_EQ(t.path_of(main, '\\'), "src\\main.cpp");
  EXPECT_EQ(t.node(main).attr, kFile);
  EXPECT_EQ(t.node(src).child_count, 1u);
}

TEST(MetadataTree, InsertErrors) {
  MetadataTree t;
  const NodeId f = *t.insert(t.root(), "f", kFile);
  EXPECT_EQ(t.insert(t.root(), "f", kFile).error(), Errc::AlreadyExists);
  EXPECT_EQ(t.insert(f, "child", kFile).error(), Errc::NotADirectory);
  EXPECT_EQ(t.insert(t.root(), "", kFile).error(), Errc::InvalidPath);
  EXPECT_EQ(t.insert(t.root(), ".", kFile).error(), Errc::InvalidPath);
  EXPECT_EQ(t.insert(t.root(), "..", kFile).error(), Errc::InvalidPath);
  EXPECT_EQ(t.insert(t.root(), "a/b", kFile).error(), Errc::InvalidPath);
  EXPECT_EQ(t.insert(t.root(), "a\\b", kFile).error(), Errc::InvalidPath);
  EXPECT_EQ(t.insert(9999, "x", kFile).error(), Errc::NotFound);
}

TEST(MetadataTree, CaseInsensitiveLookup) {
  MetadataTree t;
  const NodeId readme = *t.insert(t.root(), "README.md", kFile);
  EXPECT_EQ(t.lookup("README.md", LookupMode::Exact), readme);
  EXPECT_FALSE(t.lookup("readme.md", LookupMode::Exact).has_value());
  EXPECT_EQ(t.lookup("readme.md", LookupMode::CaseInsensitive), readme);
  EXPECT_EQ(t.lookup("ReadMe.MD", LookupMode::CaseInsensitive), readme);
  EXPECT_FALSE(t.lookup("readme.txt", LookupMode::CaseInsensitive).has_value());

  const NodeId dir = *t.insert(t.root(), "Ünïcode", kDir);
  const NodeId inner = *t.insert(dir, "File", kFile);
  EXPECT_EQ(t.lookup("ünïCODE/file", LookupMode::CaseInsensitive), inner);
}

TEST(MetadataTree, CaseCollisionsPreferExactThenFirstInserted) {
  MetadataTree t;
  const NodeId upper = *t.insert(t.root(), "Makefile", kFile);
  const NodeId lower = *t.insert(t.root(), "makefile", kFile);
  EXPECT_EQ(t.stats().case_collisions, 1u);
  EXPECT_EQ(t.lookup("Makefile", LookupMode::CaseInsensitive), upper);
  EXPECT_EQ(t.lookup("makefile", LookupMode::CaseInsensitive), lower);
  EXPECT_EQ(t.lookup("MAKEFILE", LookupMode::CaseInsensitive), upper);  // first inserted wins

  ASSERT_TRUE(t.remove(upper).has_value());
  EXPECT_EQ(t.stats().case_collisions, 0u);
  EXPECT_EQ(t.lookup("MAKEFILE", LookupMode::CaseInsensitive), lower);  // slot handed over

  const NodeId upper2 = *t.insert(t.root(), "MAKEFILE", kFile);
  EXPECT_EQ(t.stats().case_collisions, 1u);
  ASSERT_TRUE(t.remove(upper2).has_value());  // remove the shadowed one
  EXPECT_EQ(t.stats().case_collisions, 0u);
  EXPECT_EQ(t.lookup("MakeFile", LookupMode::CaseInsensitive), lower);
}

TEST(MetadataTree, RemoveSubtreeFreesNodesAndKeepsCollisionCount) {
  MetadataTree t;
  const NodeId a = *t.insert(t.root(), "a", kDir);
  const NodeId b = *t.insert(a, "b", kDir);
  (void)t.insert(b, "X", kFile);
  (void)t.insert(b, "x", kFile);  // collision inside the subtree
  (void)t.insert(a, "c", kFile);
  EXPECT_EQ(t.size(), 6u);
  EXPECT_EQ(t.stats().case_collisions, 1u);

  ASSERT_TRUE(t.remove(a).has_value());
  EXPECT_EQ(t.size(), 1u);
  EXPECT_EQ(t.stats().case_collisions, 0u);
  EXPECT_FALSE(t.valid(a));
  EXPECT_FALSE(t.valid(b));
  EXPECT_FALSE(t.lookup("a").has_value());
  EXPECT_FALSE(t.lookup("a/b/X").has_value());
  EXPECT_EQ(t.node(t.root()).child_count, 0u);

  // Freed slots are reused.
  const NodeId reused = *t.insert(t.root(), "new", kFile);
  EXPECT_LE(reused, b);
  EXPECT_EQ(t.remove(t.root()).error(), Errc::InvalidArgument);
}

TEST(MetadataTree, ReaddirOrderAndSiblingLinks) {
  MetadataTree t;
  for (const char* n : {"c", "a", "b", "d"}) (void)t.insert(t.root(), n, kFile);
  EXPECT_EQ(child_names(t, t.root()), (std::vector<std::string>{"c", "a", "b", "d"}));
  ASSERT_TRUE(t.remove(*t.lookup("a")).has_value());
  EXPECT_EQ(child_names(t, t.root()), (std::vector<std::string>{"c", "b", "d"}));
  ASSERT_TRUE(t.remove(*t.lookup("d")).has_value());  // last child
  EXPECT_EQ(child_names(t, t.root()), (std::vector<std::string>{"c", "b"}));
  ASSERT_TRUE(t.remove(*t.lookup("c")).has_value());  // first child
  EXPECT_EQ(child_names(t, t.root()), (std::vector<std::string>{"b"}));
  EXPECT_EQ(t.node(t.root()).child_count, 1u);
}

TEST(MetadataTree, Rename) {
  MetadataTree t;
  const NodeId a = *t.insert(t.root(), "a", kDir);
  const NodeId b = *t.insert(t.root(), "b", kDir);
  const NodeId f = *t.insert(a, "File.txt", kFile);
  (void)t.insert(b, "taken", kFile);

  ASSERT_TRUE(t.rename(f, b, "moved.txt").has_value());
  EXPECT_FALSE(t.lookup("a/File.txt").has_value());
  EXPECT_EQ(t.lookup("b/moved.txt"), f);
  EXPECT_EQ(t.lookup("b/MOVED.TXT", LookupMode::CaseInsensitive), f);
  EXPECT_FALSE(t.lookup("b/file.txt", LookupMode::CaseInsensitive).has_value());
  EXPECT_EQ(t.node(a).child_count, 0u);
  EXPECT_EQ(t.node(b).child_count, 2u);
  EXPECT_EQ(t.path_of(f), "b/moved.txt");

  EXPECT_EQ(t.rename(f, b, "taken").error(), Errc::AlreadyExists);
  EXPECT_TRUE(t.rename(f, b, "moved.txt").has_value());  // no-op rename
  EXPECT_EQ(t.rename(a, a, "self").error(), Errc::InvalidArgument);
  const NodeId deep = *t.insert(a, "deep", kDir);
  EXPECT_EQ(t.rename(a, deep, "loop").error(), Errc::InvalidArgument);
  EXPECT_EQ(t.rename(t.root(), a, "r").error(), Errc::InvalidArgument);
  EXPECT_EQ(t.rename(f, f, "x").error(), Errc::NotADirectory);
}

TEST(MetadataTree, UpsertAndSetAttributes) {
  MetadataTree t;
  const NodeId f = *t.upsert(t.root(), "f", kFile);
  Attributes bigger = kFile;
  bigger.size = 1000;
  EXPECT_EQ(*t.upsert(t.root(), "f", bigger), f);
  EXPECT_EQ(t.node(f).attr.size, 1000u);
  EXPECT_EQ(t.size(), 2u);

  // Directory becoming a file drops its children.
  const NodeId d = *t.insert(t.root(), "d", kDir);
  (void)t.insert(d, "child", kFile);
  ASSERT_TRUE(t.set_attributes(d, kFile).has_value());
  EXPECT_EQ(t.node(d).child_count, 0u);
  EXPECT_FALSE(t.lookup("d/child").has_value());
  EXPECT_EQ(t.size(), 3u);
  EXPECT_EQ(t.set_attributes(9999, kFile).error(), Errc::NotFound);
}

TEST(MetadataTree, PathBasedMutation) {
  MetadataTree t;
  const NodeId f = *t.upsert_path("a/b/c.txt", kFile);
  EXPECT_EQ(t.lookup("a/b/c.txt"), f);
  EXPECT_TRUE(t.node(*t.lookup("a")).is_dir());
  EXPECT_TRUE(t.node(*t.lookup("a/b")).is_dir());
  EXPECT_EQ(*t.upsert_path("\\a\\b\\c.txt", kFile), f);
  EXPECT_EQ(t.upsert_path("", kFile).error(), Errc::InvalidPath);
  EXPECT_EQ(t.upsert_path("a/b/c.txt/under-file", kFile).error(), Errc::NotADirectory);
  EXPECT_EQ(t.ensure_directory_path("a/../x").error(), Errc::InvalidPath);

  ASSERT_TRUE(t.remove_path("a/b").has_value());
  EXPECT_FALSE(t.lookup("a/b/c.txt").has_value());
  EXPECT_EQ(t.remove_path("a/b").error(), Errc::NotFound);
  EXPECT_EQ(t.size(), 2u);
}

TEST(MetadataTree, SnapshotRoundTrip) {
  MetadataTree t;
  const NodeId src = *t.insert(t.root(), "src", kDir);
  (void)t.insert(src, "a.cpp", kFile);
  const NodeId inc = *t.insert(src, "include", kDir);
  (void)t.insert(inc, "a.hpp", kFile);
  (void)t.insert(t.root(), "README", kFile);

  std::vector<SnapshotEntry> entries;
  std::vector<std::string> names;  // own the names, since SnapshotEntry only views them
  t.export_snapshot([&](const SnapshotEntry& e) {
    names.emplace_back(e.name);
    entries.push_back(e);
  });
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].name = names[i];
  ASSERT_EQ(entries.size(), 5u);
  // Breadth-first: root children first, then src's children, then include's.
  EXPECT_EQ(entries[0].name, "src");
  EXPECT_EQ(entries[0].parent, 0u);
  EXPECT_EQ(entries[1].name, "README");
  EXPECT_EQ(entries[2].name, "a.cpp");
  EXPECT_EQ(entries[2].parent, 1u);
  EXPECT_EQ(entries[3].name, "include");
  EXPECT_EQ(entries[3].parent, 1u);
  EXPECT_EQ(entries[4].name, "a.hpp");
  EXPECT_EQ(entries[4].parent, 4u);

  MetadataTree u;
  ASSERT_TRUE(u.load_snapshot(entries).has_value());
  EXPECT_EQ(u.size(), 6u);
  EXPECT_TRUE(u.lookup("src/include/a.hpp").has_value());
  EXPECT_EQ(u.node(*u.lookup("src/a.cpp")).attr, kFile);
  EXPECT_EQ(u.lookup("readme", LookupMode::CaseInsensitive), u.lookup("README"));
}

TEST(MetadataTree, LoadSnapshotRejectsForwardParents) {
  MetadataTree t;
  const std::vector<SnapshotEntry> bad{{2, "a", kFile}, {0, "b", kDir}};
  EXPECT_EQ(t.load_snapshot(bad).error(), Errc::Corrupt);  // malformed wire data is still fatal
}

TEST(MetadataTree, LoadSnapshotDropsUnrepresentableEntriesInsteadOfFailing) {
  // A single entry we cannot represent must not cost the whole tree: it is
  // dropped (with its descendants) and the rest loads. Real filesystems contain
  // such names — systemd writes `system-systemd\x2dcryptsetup.slice`, and a
  // backslash is a path separator here, so it fails valid_name().
  MetadataTree t;
  const std::vector<SnapshotEntry> entries{
      {0, "keep", kDir},            // 1
      {0, "bad\\name", kDir},       // 2 - unrepresentable, dropped
      {2, "under-bad", kFile},      // 3 - its child, dropped with it
      {1, "kept-file", kFile},      // 4 - unaffected sibling subtree
  };
  ASSERT_TRUE(t.load_snapshot(entries).has_value());
  EXPECT_EQ(t.stats().dropped, 2u);  // the bad entry and its descendant
  EXPECT_TRUE(t.lookup("keep").has_value());
  EXPECT_TRUE(t.lookup("keep/kept-file").has_value());
  EXPECT_FALSE(t.lookup("bad\\name").has_value());

  // A child of a non-directory is likewise dropped rather than fatal.
  const std::vector<SnapshotEntry> not_dir{{0, "f", kFile}, {1, "child", kFile}};
  ASSERT_TRUE(t.load_snapshot(not_dir).has_value());
  EXPECT_EQ(t.stats().dropped, 1u);
  EXPECT_TRUE(t.lookup("f").has_value());

  // Loading a clean snapshot resets the counter.
  const std::vector<SnapshotEntry> clean{{0, "ok", kFile}};
  ASSERT_TRUE(t.load_snapshot(clean).has_value());
  EXPECT_EQ(t.stats().dropped, 0u);
}

TEST(MetadataTree, LargeTree) {
  MetadataTree t;
  for (int d = 0; d < 300; ++d) {
    const NodeId dir = *t.insert(t.root(), "dir" + std::to_string(d), kDir);
    for (int f = 0; f < 100; ++f) ASSERT_TRUE(t.insert(dir, "file" + std::to_string(f) + ".cpp", kFile).has_value());
  }
  EXPECT_EQ(t.size(), 1u + 300u + 300u * 100u);
  EXPECT_TRUE(t.lookup("dir299/file99.cpp").has_value());
  EXPECT_TRUE(t.lookup("DIR299/FILE99.CPP", LookupMode::CaseInsensitive).has_value());
  EXPECT_FALSE(t.lookup("dir300/file0.cpp").has_value());
  EXPECT_EQ(t.stats().names, 1u + 300u + 100u);  // names are shared across directories
}

}  // namespace
}  // namespace wsld
