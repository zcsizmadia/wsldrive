#include "core/content_cache.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace wsld {
namespace {

namespace fs = std::filesystem;

std::span<const std::byte> bytes(std::string_view s) { return std::as_bytes(std::span{s.data(), s.size()}); }

class ContentCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = fs::temp_directory_path() / ("wsldrive-cache-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                         "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(root_);
  }
  void TearDown() override { fs::remove_all(root_); }

  fs::path root_;
};

TEST_F(ContentCacheTest, PutGetContains) {
  auto cache = ContentCache::open({.root = root_});
  ASSERT_TRUE(cache.has_value());
  EXPECT_EQ(cache->count(), 0u);

  auto d = cache->put(bytes("hello"));
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(*d, blake3_hash("hello"));
  EXPECT_TRUE(cache->contains(*d));
  EXPECT_EQ(cache->count(), 1u);
  EXPECT_EQ(cache->bytes(), 5u);

  auto got = cache->get(*d);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(got->data()), got->size()), "hello");

  // Idempotent put.
  EXPECT_EQ(*cache->put(bytes("hello")), *d);
  EXPECT_EQ(cache->count(), 1u);
  EXPECT_EQ(cache->bytes(), 5u);

  auto p = cache->path_for(*d);
  ASSERT_TRUE(p.has_value());
  EXPECT_TRUE(fs::exists(*p));
  EXPECT_EQ(p->filename().string(), d->hex());
  EXPECT_EQ(p->parent_path().filename().string(), d->hex().substr(0, 2));

  EXPECT_EQ(cache->get(blake3_hash("missing")).error(), Errc::NotFound);
  EXPECT_EQ(cache->stats().hits, 1u);
  EXPECT_EQ(cache->stats().misses, 1u);
}

TEST_F(ContentCacheTest, EmptyObject) {
  auto cache = ContentCache::open({.root = root_});
  ASSERT_TRUE(cache.has_value());
  auto d = cache->put({});
  ASSERT_TRUE(d.has_value());
  auto got = cache->get(*d);
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got->empty());
}

TEST_F(ContentCacheTest, ReopenRebuildsIndexAndCleansTemp) {
  Digest d;
  {
    auto cache = ContentCache::open({.root = root_});
    ASSERT_TRUE(cache.has_value());
    d = *cache->put(bytes("persisted content"));
    (void)cache->put(bytes("second"));
    std::ofstream(root_ / "tmp" / "leftover.0") << "junk";
  }
  auto cache = ContentCache::open({.root = root_});
  ASSERT_TRUE(cache.has_value());
  EXPECT_EQ(cache->count(), 2u);
  EXPECT_EQ(cache->bytes(), 17u + 6u);
  EXPECT_TRUE(cache->contains(d));
  EXPECT_FALSE(fs::exists(root_ / "tmp" / "leftover.0"));
  auto got = cache->get(d);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->size(), 17u);
}

TEST_F(ContentCacheTest, EraseAndEvictLru) {
  auto cache = ContentCache::open({.root = root_, .max_bytes = 25});
  ASSERT_TRUE(cache.has_value());
  const Digest a = *cache->put(bytes("aaaaaaaaaa"));  // 10
  const Digest b = *cache->put(bytes("bbbbbbbbbb"));  // 10
  const Digest c = *cache->put(bytes("cccccccccc"));  // 10 -> 30 total
  EXPECT_EQ(cache->bytes(), 30u);

  (void)cache->get(a);  // a is now the most recently used; b is the oldest
  cache->evict();
  EXPECT_LE(cache->bytes(), 25u);
  EXPECT_EQ(cache->count(), 2u);
  EXPECT_TRUE(cache->contains(a));
  EXPECT_FALSE(cache->contains(b));
  EXPECT_TRUE(cache->contains(c));
  EXPECT_FALSE(fs::exists(root_ / "objects" / b.hex().substr(0, 2) / b.hex()));

  ASSERT_TRUE(cache->erase(c).has_value());
  EXPECT_EQ(cache->erase(c).error(), Errc::NotFound);
  EXPECT_EQ(cache->count(), 1u);
  EXPECT_EQ(cache->bytes(), 10u);

  cache->evict_to(0);
  EXPECT_EQ(cache->count(), 0u);
  EXPECT_EQ(cache->bytes(), 0u);
}

TEST_F(ContentCacheTest, ExternallyDeletedObjectIsDroppedOnGet) {
  auto cache = ContentCache::open({.root = root_});
  ASSERT_TRUE(cache.has_value());
  const Digest d = *cache->put(bytes("volatile"));
  fs::remove(*cache->path_for(d));
  EXPECT_EQ(cache->get(d).error(), Errc::NotFound);
  EXPECT_FALSE(cache->contains(d));
  EXPECT_EQ(cache->bytes(), 0u);
}

}  // namespace
}  // namespace wsld
