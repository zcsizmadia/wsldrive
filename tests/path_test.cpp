#include "core/path.hpp"

#include <gtest/gtest.h>

namespace wsld {
namespace {

TEST(Path, Normalize) {
  EXPECT_EQ(normalize_path("a/b/c"), "a/b/c");
  EXPECT_EQ(normalize_path("/a//b\\c/"), "a/b/c");
  EXPECT_EQ(normalize_path("\\\\a\\.\\b"), "a/b");
  EXPECT_EQ(normalize_path("./a/./b/."), "a/b");
  EXPECT_EQ(normalize_path(""), "");
  EXPECT_EQ(normalize_path("/"), "");
  EXPECT_EQ(normalize_path("a/../b"), "a/../b");  // kept verbatim by design
}

TEST(Path, SplitParent) {
  auto [p1, l1] = split_parent("a/b/c");
  EXPECT_EQ(p1, "a/b");
  EXPECT_EQ(l1, "c");
  auto [p2, l2] = split_parent("c");
  EXPECT_EQ(p2, "");
  EXPECT_EQ(l2, "c");
  auto [p3, l3] = split_parent("");
  EXPECT_EQ(p3, "");
  EXPECT_EQ(l3, "");
}

TEST(Path, IsUnder) {
  EXPECT_TRUE(path_is_under("a/b", "a"));
  EXPECT_TRUE(path_is_under("a", "a"));
  EXPECT_TRUE(path_is_under("anything", ""));
  EXPECT_FALSE(path_is_under("ab", "a"));
  EXPECT_FALSE(path_is_under("a", "a/b"));
  EXPECT_FALSE(path_is_under("b/a", "a"));
}

}  // namespace
}  // namespace wsld
