#include "core/win_names.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wsld {
namespace {

// U+F000+b encodes as EF 8x 8x; helper to build the expected escaped byte.
std::string pua(unsigned char b) {
  const char32_t cp = 0xF000 + b;
  std::string s;
  s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
  s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
  s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  return s;
}

TEST(WinNames, PlainNamesUnchanged) {
  for (const char* n : {"main.cpp", "README", "a-b_c.123", "Ünïcode", "日本語"}) {
    EXPECT_FALSE(needs_escaping(n));
    EXPECT_EQ(escape_for_windows(n), n);
    EXPECT_EQ(unescape_from_windows(n), n);
  }
}

TEST(WinNames, ReservedCharactersRoundTrip) {
  const std::string raw = "a:b?c*d<e>f|g\"h";
  EXPECT_TRUE(needs_escaping(raw));
  const std::string shown = escape_for_windows(raw);
  EXPECT_EQ(shown.find(':'), std::string::npos);  // no raw reserved chars remain
  EXPECT_EQ(shown.find('?'), std::string::npos);
  EXPECT_NE(shown.find(pua(':')), std::string::npos);
  EXPECT_EQ(unescape_from_windows(shown), raw);
}

TEST(WinNames, ControlCharacters) {
  std::string raw = "tab\tnl";
  const std::string shown = escape_for_windows(raw);
  EXPECT_EQ(shown.find('\t'), std::string::npos);
  EXPECT_EQ(unescape_from_windows(shown), raw);
}

TEST(WinNames, TrailingDotAndSpace) {
  EXPECT_TRUE(needs_escaping("name."));
  EXPECT_TRUE(needs_escaping("name "));
  EXPECT_FALSE(needs_escaping("na.me"));  // interior dot is fine
  const std::string shown = escape_for_windows("name.");
  EXPECT_EQ(shown, std::string("name") + pua('.'));
  EXPECT_EQ(unescape_from_windows(shown), "name.");
  EXPECT_EQ(unescape_from_windows(escape_for_windows("name ")), "name ");
  // An interior dot stays literal; only the trailing one is escaped.
  EXPECT_EQ(escape_for_windows("a.b."), std::string("a.b") + pua('.'));
}

TEST(WinNames, UnescapeLeavesForeignPuaBytesForRawText) {
  // Round-trip stability: escaping then unescaping is identity for any input.
  for (const std::string& raw : {std::string("weird:name "), std::string("\x01\x02*"), std::string("plain")})
    EXPECT_EQ(unescape_from_windows(escape_for_windows(raw)), raw);
}

}  // namespace
}  // namespace wsld
