#include "core/unicode.hpp"

#include <gtest/gtest.h>

namespace wsld {
namespace {

TEST(Casefold, Ascii) {
  EXPECT_EQ(casefold("Hello World.TXT"), "hello world.txt");
  EXPECT_EQ(casefold(""), "");
  EXPECT_EQ(casefold("already lower 123 -_"), "already lower 123 -_");
}

TEST(Casefold, Latin1AndExtendedA) {
  EXPECT_EQ(casefold("ÀÉÎÕÜ"), "àéîõü");
  EXPECT_EQ(casefold("×"), "×");  // multiplication sign is not a letter
  EXPECT_EQ(casefold("ĀĂĄĆ"), "āăąć");
  EXPECT_EQ(casefold("ĹĽŃŇ"), "ĺľńň");  // odd-upper range
  EXPECT_EQ(casefold("ŹŻŽ"), "źżž");
  EXPECT_EQ(casefold("Ÿ"), "ÿ");
  EXPECT_EQ(casefold("ſ"), "s");
}

TEST(Casefold, GreekAndCyrillic) {
  EXPECT_EQ(casefold("ΑΒΓΩ"), "αβγω");
  EXPECT_EQ(casefold("ПРИВЕТ"), "привет");
  EXPECT_EQ(casefold("ЁЙ"), "ёй");
}

TEST(Casefold, PassesThroughUnknownScriptsAndInvalidUtf8) {
  EXPECT_EQ(casefold("日本語"), "日本語");
  const std::string bad("\xFF\xC0\x41", 3);  // invalid lead bytes followed by 'A'
  EXPECT_EQ(casefold(bad), std::string("\xFF\xC0\x61", 3));
  const std::string truncated("\xE6\x97", 2);  // incomplete 3-byte sequence
  EXPECT_EQ(casefold(truncated), truncated);
}

TEST(Casefold, RejectsOverlongEncodings) {
  const std::string overlong_a("\xC1\x81", 2);  // overlong 'A'
  EXPECT_EQ(casefold(overlong_a), overlong_a);   // must not become 'a'
}

TEST(Casefold, IsAscii) {
  EXPECT_TRUE(is_ascii("abc/DEF.txt"));
  EXPECT_FALSE(is_ascii("é"));
}

}  // namespace
}  // namespace wsld
