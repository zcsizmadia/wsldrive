#include "core/string_pool.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wsld {
namespace {

TEST(StringPool, InternIsIdempotent) {
  StringPool p;
  const NameId a = p.intern("hello");
  const NameId b = p.intern("hello");
  const NameId c = p.intern("world");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_EQ(p.size(), 2u);
  EXPECT_EQ(p.view(a), "hello");
  EXPECT_EQ(p.view(c), "world");
}

TEST(StringPool, FindDoesNotIntern) {
  StringPool p;
  EXPECT_FALSE(p.find("x").has_value());
  EXPECT_EQ(p.size(), 0u);
  const NameId id = p.intern("x");
  ASSERT_TRUE(p.find("x").has_value());
  EXPECT_EQ(*p.find("x"), id);
}

TEST(StringPool, EmptyStringIsValid) {
  StringPool p;
  const NameId id = p.intern("");
  EXPECT_EQ(p.view(id), "");
  EXPECT_EQ(p.intern(""), id);
}

TEST(StringPool, ViewsStayValidAcrossGrowth) {
  StringPool p;
  std::vector<std::string_view> views;
  std::vector<std::string> expected;
  for (int i = 0; i < 200000; ++i) {
    expected.push_back("name-" + std::to_string(i));
    views.push_back(p.view(p.intern(expected.back())));
  }
  for (std::size_t i = 0; i < views.size(); ++i) EXPECT_EQ(views[i], expected[i]);
  EXPECT_EQ(p.size(), expected.size());
}

TEST(StringPool, OversizedNames) {
  StringPool p;
  const std::string big(3u << 20, 'x');
  const NameId a = p.intern(big);
  const NameId b = p.intern("small");
  const NameId c = p.intern(std::string(2u << 20, 'y'));
  EXPECT_EQ(p.view(a), big);
  EXPECT_EQ(p.view(b), "small");
  EXPECT_EQ(p.view(c).size(), 2u << 20);
  EXPECT_EQ(p.intern(big), a);
}

TEST(StringPool, ClearResets) {
  StringPool p;
  p.intern("a");
  p.clear();
  EXPECT_EQ(p.size(), 0u);
  EXPECT_FALSE(p.find("a").has_value());
  EXPECT_EQ(p.intern("b"), 0u);
}

}  // namespace
}  // namespace wsld
