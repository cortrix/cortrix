#include <gtest/gtest.h>

#include <sqlite3.h>

#include <string>
#include <vector>

#include "cortrix/catalog/bloom_filter.h"
#include "cortrix/catalog/catalog_db.h"

// S4.1 coverage: BloomFilter (catalog dedup accelerator).
namespace cortrix::catalog {
namespace {

// BloomFilter has std::atomic members → not copyable/movable, so it can't be
// returned by value. Construct in place and mark ready (skips the rebuild path
// for pure-logic tests) via this macro.
#define READY_BF(var, bytes, fp) \
    BloomFilter var((bytes), (fp));  \
    var.MarkReady(1)

TEST(BloomFilterTest, AddedKeysAlwaysMightContain) {
    READY_BF(bf, 1024 * 1024, 0.01);
    std::vector<std::string> keys;
    for (int i = 0; i < 5000; ++i) keys.push_back("file-hash-" + std::to_string(i));
    for (const auto& k : keys) ASSERT_TRUE(bf.Add(k).ok());

    // No false negatives — every added key must report present.
    for (const auto& k : keys) {
        EXPECT_TRUE(bf.MightContain(k)) << "false negative for " << k;
    }
    EXPECT_EQ(bf.EstimatedCount(), keys.size());
}

TEST(BloomFilterTest, AbsentKeyUsuallyFalse) {
    READY_BF(bf, 1024 * 1024, 0.01);
    for (int i = 0; i < 1000; ++i) bf.Add("present-" + std::to_string(i));
    // A never-added key should (almost always) be absent. With 1% target FP and
    // a 1MB filter at this fill the rate is ~0, so a single check is safe.
    EXPECT_FALSE(bf.MightContain("definitely-not-added-key"));
}

TEST(BloomFilterTest, LowFalsePositiveRateOnDisjointSet) {
    READY_BF(bf, 1024 * 1024, 0.01);
    for (int i = 0; i < 5000; ++i) bf.Add("in-" + std::to_string(i));
    int fp = 0;
    const int trials = 5000;
    for (int i = 0; i < trials; ++i) {
        if (bf.MightContain("out-" + std::to_string(i))) ++fp;
    }
    // Comfortably under a loose 5% bound (target 1%, generous to avoid flakiness).
    EXPECT_LT(fp, trials / 20) << "fp=" << fp << "/" << trials;
}

TEST(BloomFilterTest, UnreadyFilterIsConservativelyTrue) {
    BloomFilter bf(1024 * 1024, 0.01);  // not marked ready
    EXPECT_FALSE(bf.IsReady());
    // While rebuilding, MightContain must say "maybe" (true) for ANY key so the
    // caller falls back to the catalog (§9.2).
    EXPECT_TRUE(bf.MightContain("anything"));
    EXPECT_TRUE(bf.MightContain("never-added"));
}

TEST(BloomFilterTest, KSelectedFromTargetRate) {
    // Lower target FP → more hash functions → fewer false positives.
    READY_BF(loose, 256 * 1024, 0.1);
    READY_BF(tight, 256 * 1024, 0.001);
    for (int i = 0; i < 3000; ++i) {
        std::string k = "x-" + std::to_string(i);
        loose.Add(k);
        tight.Add(k);
    }
    auto count_fp = [](BloomFilter& bf) {
        int fp = 0;
        for (int i = 0; i < 3000; ++i)
            if (bf.MightContain("y-" + std::to_string(i))) ++fp;
        return fp;
    };
    EXPECT_GE(count_fp(loose), count_fp(tight));
}

TEST(BloomFilterTest, EstimatedFprGrowsWithFill) {
    READY_BF(bf, 64 * 1024, 0.01);
    EXPECT_DOUBLE_EQ(bf.EstimatedFalsePositiveRate(), 0.0);  // empty
    for (int i = 0; i < 1000; ++i) bf.Add("k-" + std::to_string(i));
    double a = bf.EstimatedFalsePositiveRate();
    for (int i = 1000; i < 5000; ++i) bf.Add("k-" + std::to_string(i));
    double b = bf.EstimatedFalsePositiveRate();
    EXPECT_GT(a, 0.0);
    EXPECT_GT(b, a);  // more items → higher theoretical FPR
}

TEST(BloomFilterTest, RuntimeFpStatsTrackReports) {
    READY_BF(bf, 1024 * 1024, 0.01);
    bf.Add("a");
    EXPECT_TRUE(bf.MightContain("a"));   // a positive hit → total_checks++
    bf.ReportFalsePositive();            // caller verified one hit was false
    auto s = bf.GetFalsePositiveStats();
    EXPECT_GE(s.total_checks, 1u);
    EXPECT_EQ(s.false_positive_count, 1u);
    EXPECT_GT(s.runtime_fpr, 0.0);
}

TEST(BloomFilterTest, LoadFromCatalogRebuildsFromFileLocations) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    // Seed tenants/units (FK targets) + a few file_locations rows.
    ASSERT_EQ(sqlite3_exec(catalog.db(),
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t1',0);"
        "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);"
        "INSERT INTO file_locations(file_hash, unit_id, ns_id, tenant_id, first_seen_at, created_at) "
        "VALUES('fh-A','u1','ns1','t1',0,0),('fh-B','u1','ns1','t1',0,0),('fh-C','u1','ns1','t1',0,0);",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(catalog.db());

    BloomFilter bf(1024 * 1024, 0.01);
    EXPECT_FALSE(bf.IsReady());
    ASSERT_TRUE(bf.LoadFromCatalog(catalog.db()).ok());

    EXPECT_TRUE(bf.IsReady());
    EXPECT_FLOAT_EQ(bf.GetLoadProgress(), 1.0f);
    ASSERT_TRUE(bf.LastRebuildAt().has_value());
    EXPECT_EQ(bf.EstimatedCount(), 3u);
    // The seeded hashes are present; an unseeded one is (almost surely) absent.
    EXPECT_TRUE(bf.MightContain("fh-A"));
    EXPECT_TRUE(bf.MightContain("fh-B"));
    EXPECT_TRUE(bf.MightContain("fh-C"));
    EXPECT_FALSE(bf.MightContain("fh-NEVER"));
}

TEST(BloomFilterTest, LoadFromCatalogEmptyTableReadyWithZero) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    BloomFilter bf(64 * 1024, 0.01);
    ASSERT_TRUE(bf.LoadFromCatalog(catalog.db()).ok());
    EXPECT_TRUE(bf.IsReady());
    EXPECT_EQ(bf.EstimatedCount(), 0u);
}

TEST(BloomFilterTest, LoadFromCatalogNullDbIsInvalid) {
    BloomFilter bf(64 * 1024, 0.01);
    Status st = bf.LoadFromCatalog(nullptr);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace cortrix::catalog
