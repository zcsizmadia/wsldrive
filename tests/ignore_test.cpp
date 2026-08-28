#include "core/ignore.hpp"

#include <gtest/gtest.h>

namespace wsld {
namespace {

TEST(IgnoreRules, EmptyIgnoresNothing) {
  IgnoreRules r = IgnoreRules::parse("");
  EXPECT_TRUE(r.empty());
  EXPECT_FALSE(r.ignored("anything", false));
  EXPECT_FALSE(r.ignored("a/b/c", true));
}

TEST(IgnoreRules, CommentsAndBlankLines) {
  IgnoreRules r = IgnoreRules::parse("# a comment\n\n   \n#another\n");
  EXPECT_TRUE(r.empty());
}

TEST(IgnoreRules, DirectoryPatternAnyDepth) {
  IgnoreRules r = IgnoreRules::parse("node_modules/\n");
  EXPECT_TRUE(r.ignored("node_modules", true));
  EXPECT_FALSE(r.ignored("node_modules", false));  // dir-only: a file of that name is kept
  EXPECT_TRUE(r.ignored("src/node_modules", true));
  EXPECT_TRUE(r.ignored("a/node_modules/pkg/index.js", false));  // under an ignored dir
  EXPECT_FALSE(r.ignored("src/keep", true));
}

TEST(IgnoreRules, BasenameGlob) {
  IgnoreRules r = IgnoreRules::parse("*.log\n");
  EXPECT_TRUE(r.ignored("app.log", false));
  EXPECT_TRUE(r.ignored("a/b/app.log", false));
  EXPECT_FALSE(r.ignored("app.log.1", false));
  EXPECT_FALSE(r.ignored("applog", false));
}

TEST(IgnoreRules, PlainNameMatchesFileOrDirAnyDepth) {
  IgnoreRules r = IgnoreRules::parse("foo\n");
  EXPECT_TRUE(r.ignored("foo", false));
  EXPECT_TRUE(r.ignored("foo", true));
  EXPECT_TRUE(r.ignored("a/foo/b", false));  // foo is an interior directory
  EXPECT_FALSE(r.ignored("foobar", false));
  EXPECT_FALSE(r.ignored("barfoo", false));
}

TEST(IgnoreRules, AnchoredSingleSegment) {
  IgnoreRules r = IgnoreRules::parse("/build\n");
  EXPECT_TRUE(r.ignored("build", true));
  EXPECT_TRUE(r.ignored("build/out.o", false));  // under the anchored dir prefix
  EXPECT_FALSE(r.ignored("src/build", true));     // anchored: only at the root
}

TEST(IgnoreRules, AnchoredMultiSegmentDirOnly) {
  IgnoreRules r = IgnoreRules::parse("a/b/\n");
  EXPECT_TRUE(r.ignored("a/b", true));
  EXPECT_TRUE(r.ignored("a/b/c.txt", false));
  EXPECT_FALSE(r.ignored("a/b", false));  // dir-only and no deeper: kept
  EXPECT_FALSE(r.ignored("x/a/b", true));  // anchored elsewhere
}

TEST(IgnoreRules, InteriorSlashImpliesAnchored) {
  IgnoreRules r = IgnoreRules::parse("src/*.tmp\n");
  EXPECT_TRUE(r.ignored("src/a.tmp", false));
  EXPECT_FALSE(r.ignored("other/a.tmp", false));
  EXPECT_FALSE(r.ignored("src/sub/a.tmp", false));  // '*' does not cross '/'
}

TEST(IgnoreRules, MultiplePatterns) {
  IgnoreRules r = IgnoreRules::parse("target/\n*.log\n/.git/\n");
  EXPECT_TRUE(r.ignored("target", true));
  EXPECT_TRUE(r.ignored("deep/x.log", false));
  EXPECT_TRUE(r.ignored(".git", true));
  EXPECT_TRUE(r.ignored(".git/config", false));
  EXPECT_FALSE(r.ignored("src/main.cpp", false));
}

}  // namespace
}  // namespace wsld
