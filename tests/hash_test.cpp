#include "core/hash.hpp"
#include "core/hash_util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

namespace wsld {
namespace {

// Official BLAKE3 test vectors.
constexpr const char* kEmptyHex = "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";
constexpr const char* kAbcHex = "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";

TEST(Blake3, KnownVectors) {
  EXPECT_EQ(blake3_hash("").hex(), kEmptyHex);
  EXPECT_EQ(blake3_hash("abc").hex(), kAbcHex);
}

TEST(Blake3, IncrementalMatchesOneShot) {
  const std::string data(100000, 'z');
  Blake3Hasher h;
  for (std::size_t i = 0; i < data.size(); i += 7777) h.update(std::string_view(data).substr(i, 7777));
  EXPECT_EQ(h.finalize(), blake3_hash(data));
  h.reset();
  h.update("abc");
  EXPECT_EQ(h.finalize().hex(), kAbcHex);
}

TEST(Digest, HexRoundTrip) {
  const Digest d = blake3_hash("round trip");
  const auto parsed = Digest::from_hex(d.hex());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, d);
  EXPECT_FALSE(Digest::from_hex("").has_value());
  EXPECT_FALSE(Digest::from_hex(std::string(63, 'a')).has_value());
  EXPECT_FALSE(Digest::from_hex(std::string(64, 'g')).has_value());
  auto upper = d.hex();
  for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_EQ(Digest::from_hex(upper), d);
}

TEST(HashBytes, DistinctAndStable) {
  EXPECT_EQ(hash_string("abc"), hash_string("abc"));
  EXPECT_NE(hash_string("abc"), hash_string("abd"));
  EXPECT_NE(hash_string("abc"), hash_string(std::string_view("abc\0", 4)));  // length is part of the hash
  EXPECT_NE(hash_string(""), hash_string(std::string_view("\0", 1)));
  EXPECT_NE(hash_string(std::string_view("\0\0", 2)), hash_string(std::string_view("\0\0\0", 3)));
  // No pathological collisions across a realistic name set.
  std::unordered_set<std::uint64_t> seen;
  for (int i = 0; i < 100000; ++i) seen.insert(hash_string("file_" + std::to_string(i) + ".cpp"));
  EXPECT_EQ(seen.size(), 100000u);
}

}  // namespace
}  // namespace wsld
