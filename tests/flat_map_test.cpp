#include "core/flat_map.hpp"

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

namespace wsld {
namespace {

TEST(U64Map, EmptyFind) {
  U64Map m;
  EXPECT_EQ(m.find(42), nullptr);
  EXPECT_FALSE(m.erase(42));
  EXPECT_EQ(m.size(), 0u);
}

TEST(U64Map, InsertFindErase) {
  U64Map m;
  EXPECT_TRUE(m.insert_or_assign(1, 10));
  EXPECT_TRUE(m.insert_or_assign(2, 20));
  EXPECT_FALSE(m.insert_or_assign(1, 11));  // overwrite
  ASSERT_NE(m.find(1), nullptr);
  EXPECT_EQ(*m.find(1), 11u);
  EXPECT_EQ(*m.find(2), 20u);
  EXPECT_EQ(m.size(), 2u);
  EXPECT_TRUE(m.erase(1));
  EXPECT_EQ(m.find(1), nullptr);
  EXPECT_EQ(m.size(), 1u);
  EXPECT_FALSE(m.erase(1));
}

TEST(U64Map, InsertIfAbsentKeepsFirst) {
  U64Map m;
  EXPECT_TRUE(m.insert_if_absent(7, 1));
  EXPECT_FALSE(m.insert_if_absent(7, 2));
  EXPECT_EQ(*m.find(7), 1u);
}

TEST(U64Map, TombstoneReuseAndRehashKeepsEntries) {
  U64Map m;
  // Churn: insert and erase far more keys than the capacity to exercise
  // tombstone reuse and in-place rehash.
  for (std::uint64_t round = 0; round < 50; ++round) {
    for (std::uint64_t k = 0; k < 100; ++k) EXPECT_TRUE(m.insert_or_assign(round * 1000 + k, static_cast<std::uint32_t>(k)));
    for (std::uint64_t k = 0; k < 100; ++k) EXPECT_TRUE(m.erase(round * 1000 + k));
    EXPECT_EQ(m.size(), 0u);
  }
  EXPECT_LE(m.capacity(), 512u) << "capacity must not grow when the live set stays small";
}

TEST(U64Map, MatchesReferenceUnderRandomOps) {
  std::mt19937_64 rng(12345);
  U64Map m;
  std::unordered_map<std::uint64_t, std::uint32_t> ref;
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t key = rng() % 5000;
    const auto value = static_cast<std::uint32_t>(rng());
    switch (rng() % 3) {
      case 0:
      case 1: {
        const bool inserted = m.insert_or_assign(key, value);
        EXPECT_EQ(inserted, !ref.contains(key));
        ref[key] = value;
        break;
      }
      default: {
        const bool erased = m.erase(key);
        EXPECT_EQ(erased, ref.erase(key) == 1);
        break;
      }
    }
  }
  EXPECT_EQ(m.size(), ref.size());
  for (const auto& [k, v] : ref) {
    ASSERT_NE(m.find(k), nullptr) << k;
    EXPECT_EQ(*m.find(k), v);
  }
  std::size_t visited = 0;
  m.for_each([&](std::uint64_t k, std::uint32_t v) {
    ++visited;
    EXPECT_EQ(ref.at(k), v);
  });
  EXPECT_EQ(visited, ref.size());
}

TEST(U64Map, ReserveAvoidsRehash) {
  U64Map m;
  m.reserve(1000);
  const std::size_t cap = m.capacity();
  for (std::uint64_t k = 0; k < 1000; ++k) m.insert_or_assign(k, 0);
  EXPECT_EQ(m.capacity(), cap);
}

}  // namespace
}  // namespace wsld
