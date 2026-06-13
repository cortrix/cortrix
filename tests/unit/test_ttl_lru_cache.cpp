#include <gtest/gtest.h>

#include <string>

#include "cortrix/catalog/ttl_lru_cache.h"

// S2.2 coverage: the bounded TTL+LRU cache fronting INSRouter (F12 §6.3). Time is
// injected so expiry/eviction are deterministic.
namespace cortrix::catalog {
namespace {

TEST(TtlLruCacheTest, HitWithinTtlMissAfter) {
    TtlLruCache<std::string, int> c(10, 60000);
    c.Put("a", 1, 1000);
    auto hit = c.Get("a", 1000 + 59999);  // just under TTL
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, 1);
    // at exactly insert + ttl → expired.
    EXPECT_FALSE(c.Get("a", 1000 + 60000).has_value());
    // expired entry was evicted on the miss.
    EXPECT_EQ(c.size(), 0u);
}

TEST(TtlLruCacheTest, MissForUnknownKey) {
    TtlLruCache<std::string, int> c(10, 60000);
    EXPECT_FALSE(c.Get("nope", 0).has_value());
}

TEST(TtlLruCacheTest, LruEvictionOverCapacity) {
    TtlLruCache<std::string, int> c(2, 60000);
    c.Put("a", 1, 0);
    c.Put("b", 2, 0);
    // touch "a" so "b" becomes LRU.
    ASSERT_TRUE(c.Get("a", 1).has_value());
    c.Put("c", 3, 1);  // evicts LRU = "b"
    EXPECT_TRUE(c.Get("a", 2).has_value());
    EXPECT_FALSE(c.Get("b", 2).has_value());
    EXPECT_TRUE(c.Get("c", 2).has_value());
    EXPECT_EQ(c.size(), 2u);
}

TEST(TtlLruCacheTest, PutOverwritesAndRestamps) {
    TtlLruCache<std::string, int> c(10, 60000);
    c.Put("a", 1, 0);
    c.Put("a", 2, 30000);            // overwrite + restamp
    auto hit = c.Get("a", 80000);    // 80000 - 30000 = 50000 < ttl → still live
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, 2);
    EXPECT_EQ(c.size(), 1u);
}

TEST(TtlLruCacheTest, InvalidateAndClear) {
    TtlLruCache<std::string, int> c(10, 60000);
    c.Put("a", 1, 0);
    c.Put("b", 2, 0);
    c.Invalidate("a");
    EXPECT_FALSE(c.Get("a", 0).has_value());
    EXPECT_TRUE(c.Get("b", 0).has_value());
    c.Clear();
    EXPECT_EQ(c.size(), 0u);
    EXPECT_FALSE(c.Get("b", 0).has_value());
}

TEST(TtlLruCacheTest, ZeroCapacityStoresNothing) {
    TtlLruCache<std::string, int> c(0, 60000);
    c.Put("a", 1, 0);
    EXPECT_EQ(c.size(), 0u);
    EXPECT_FALSE(c.Get("a", 0).has_value());
}

}  // namespace
}  // namespace cortrix::catalog
