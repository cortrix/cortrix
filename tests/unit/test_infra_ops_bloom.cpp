#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cmath>
#include <string>
#include <vector>

#include "cortrix/catalog/bloom_filter.h"
#include "cortrix/catalog/catalog_db.h"

// BREADTH op-matrix coverage for BloomFilter (catalog). Complements
// test_bloom_filter.cpp with: ctor edge inputs (capacity 0, target_fp_rate out of
// (0,1) -> default 0.01, k-clamp extremes observed via behavior), add/contains
// no-false-negative + disjoint FP-stat matrices across many key-set sizes, runtime
// FP-stat accumulation, and LoadFromCatalog prepare-failure / null-db arms.
//
// Suite names are globally unique: BloomOpsMatrix* (no clash with BloomFilterTest).
namespace cortrix::catalog {
namespace {

// BloomFilter has atomic members -> non-copyable/movable; build in place + ready.
#define OPS_READY_BF(var, bytes, fp) \
    BloomFilter var((bytes), (fp));  \
    var.MarkReady(1)

// -- ctor edge inputs: capacity_bytes matrix ---------------------------------
// Even a 0-byte capacity yields a usable (tiny) filter: ctor floors to 1 byte ->
// >=64 bits, so Add/MightContain never crash and added keys are still present.
class BloomOpsMatrixCapacityCtor : public ::testing::TestWithParam<size_t> {};

TEST_P(BloomOpsMatrixCapacityCtor, AddedKeyAlwaysPresentRegardlessOfCapacity) {
    OPS_READY_BF(bf, GetParam(), 0.01);
    // No false negatives for the keys we actually added, at any capacity.
    for (int i = 0; i < 50; ++i) {
        std::string k = "cap-key-" + std::to_string(i);
        ASSERT_TRUE(bf.Add(k).ok());
    }
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(bf.MightContain("cap-key-" + std::to_string(i)));
    }
    EXPECT_EQ(bf.EstimatedCount(), 50u);
}

INSTANTIATE_TEST_SUITE_P(Caps, BloomOpsMatrixCapacityCtor,
                         ::testing::Values<size_t>(0, 1, 8, 64, 1024, 16 * 1024,
                                                   256 * 1024, 1024 * 1024));

// -- ctor edge inputs: target_fp_rate clamp/default matrix -------------------
// p outside (0,1) is replaced by 0.01 in the ctor; in-range values pick k.
// We can't read num_hashes_ directly, but the EFPR formula and no-false-negative
// invariant must hold for every input, including the clamped/default ones.
struct FpCase { double p; };
class BloomOpsMatrixFpRateCtor : public ::testing::TestWithParam<FpCase> {};

TEST_P(BloomOpsMatrixFpRateCtor, ConstructsUsableFilterForAnyTargetRate) {
    OPS_READY_BF(bf, 256 * 1024, GetParam().p);
    EXPECT_DOUBLE_EQ(bf.EstimatedFalsePositiveRate(), 0.0);  // empty -> 0
    for (int i = 0; i < 200; ++i) bf.Add("p-key-" + std::to_string(i));
    // No false negatives whatever k the ctor picked.
    for (int i = 0; i < 200; ++i) {
        EXPECT_TRUE(bf.MightContain("p-key-" + std::to_string(i)));
    }
    // EFPR is a valid probability in [0,1]. For very-high-k (tiny target p) the
    // theoretical rate at this low fill can underflow to exactly 0.0 in double,
    // so we bound it as a valid probability rather than strictly positive.
    double efpr = bf.EstimatedFalsePositiveRate();
    EXPECT_GE(efpr, 0.0);
    EXPECT_LE(efpr, 1.0);
}

INSTANTIATE_TEST_SUITE_P(
    Rates, BloomOpsMatrixFpRateCtor,
    ::testing::Values(
        // out-of-range -> defaulted to 0.01 (k=7)
        FpCase{-1.0}, FpCase{-0.0001}, FpCase{0.0}, FpCase{1.0}, FpCase{2.0},
        FpCase{1e9}, FpCase{std::nan("")},
        // boundary-ish small/large valid values -> k clamp extremes
        FpCase{0.999999}, FpCase{0.5}, FpCase{0.25}, FpCase{0.1}, FpCase{0.05},
        FpCase{0.01}, FpCase{0.001}, FpCase{0.0001}, FpCase{1e-6}, FpCase{1e-9},
        FpCase{1e-12}, FpCase{1e-15}, FpCase{1e-300}));

// -- k-clamp extremes via behavior: tighter p -> not more false positives ----
// p so small that k saturates at 30 must still behave (no crash, no FN), and a
// near-1 p (k clamps to 1) yields more-or-equal false positives than tight p.
TEST(BloomOpsMatrixKClamp, ExtremeLowRateClampsKHigh) {
    OPS_READY_BF(bf, 256 * 1024, 1e-18);  // -log2(p) far above 30 -> k=30
    for (int i = 0; i < 500; ++i) bf.Add("kc-" + std::to_string(i));
    for (int i = 0; i < 500; ++i)
        EXPECT_TRUE(bf.MightContain("kc-" + std::to_string(i)));
}

TEST(BloomOpsMatrixKClamp, LooseRateGivesGeOrEqualFpsThanTight) {
    OPS_READY_BF(loose, 128 * 1024, 0.5);    // k clamps to 1
    OPS_READY_BF(tight, 128 * 1024, 1e-9);   // k clamps to 30
    for (int i = 0; i < 2000; ++i) {
        std::string k = "kk-" + std::to_string(i);
        loose.Add(k);
        tight.Add(k);
    }
    auto fp = [](BloomFilter& bf) {
        int n = 0;
        for (int i = 0; i < 2000; ++i)
            if (bf.MightContain("zz-" + std::to_string(i))) ++n;
        return n;
    };
    EXPECT_GE(fp(loose), fp(tight));
}

// -- add/contains no-false-negative over key-set-size matrix ------------------
class BloomOpsMatrixNoFalseNegative : public ::testing::TestWithParam<int> {};

TEST_P(BloomOpsMatrixNoFalseNegative, EveryAddedKeyReportsPresent) {
    const int n = GetParam();
    OPS_READY_BF(bf, 1024 * 1024, 0.01);
    for (int i = 0; i < n; ++i) bf.Add("nfn-" + std::to_string(i));
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(bf.MightContain("nfn-" + std::to_string(i)))
            << "false negative at i=" << i << " n=" << n;
    }
    EXPECT_EQ(bf.EstimatedCount(), static_cast<size_t>(n));
}

INSTANTIATE_TEST_SUITE_P(Sizes, BloomOpsMatrixNoFalseNegative,
                         ::testing::Values(0, 1, 2, 3, 4, 5, 8, 10, 16, 25, 32,
                                           50, 64, 100, 128, 200, 256, 500, 1000,
                                           1500, 2000, 3000, 5000, 7500, 10000,
                                           15000, 20000, 30000, 50000));

// -- disjoint-set false-positive-rate stays bounded over size matrix ----------
class BloomOpsMatrixDisjointFp : public ::testing::TestWithParam<int> {};

TEST_P(BloomOpsMatrixDisjointFp, DisjointSetUnderLooseBound) {
    const int n = GetParam();
    OPS_READY_BF(bf, 4 * 1024 * 1024, 0.01);  // large filter keeps real FPR tiny
    for (int i = 0; i < n; ++i) bf.Add("in-" + std::to_string(i));
    int fp = 0;
    const int trials = 5000;
    for (int i = 0; i < trials; ++i)
        if (bf.MightContain("out-" + std::to_string(i))) ++fp;
    // Generous 5% ceiling (target 1%, large filter) to avoid flakiness.
    EXPECT_LT(fp, trials / 20) << "fp=" << fp << " n=" << n;
}

INSTANTIATE_TEST_SUITE_P(Sizes, BloomOpsMatrixDisjointFp,
                         ::testing::Values(100, 250, 500, 1000, 1500, 2000, 3000,
                                           5000, 7500, 10000, 15000, 20000, 30000,
                                           40000));

// -- EstimatedFalsePositiveRate monotonic over fill matrix -------------------
TEST(BloomOpsMatrixEfpr, MonotonicNonDecreasingWithFill) {
    OPS_READY_BF(bf, 64 * 1024, 0.01);
    double prev = bf.EstimatedFalsePositiveRate();
    EXPECT_DOUBLE_EQ(prev, 0.0);
    for (int step = 1; step <= 10; ++step) {
        for (int i = 0; i < 500; ++i)
            bf.Add("efpr-" + std::to_string(step) + "-" + std::to_string(i));
        double cur = bf.EstimatedFalsePositiveRate();
        EXPECT_GE(cur, prev) << "non-monotonic at step " << step;
        prev = cur;
    }
}

// -- runtime FP stats accumulate over a report-count matrix ------------------
class BloomOpsMatrixRuntimeFp : public ::testing::TestWithParam<int> {};

TEST_P(BloomOpsMatrixRuntimeFp, ReportsAccumulate) {
    const int reports = GetParam();
    OPS_READY_BF(bf, 1024 * 1024, 0.01);
    bf.Add("hit");
    // Generate at least `reports` positive hits (denominator) + reports FPs.
    for (int i = 0; i < reports; ++i) {
        EXPECT_TRUE(bf.MightContain("hit"));  // each positive hit bumps total_checks
        bf.ReportFalsePositive();
    }
    auto s = bf.GetFalsePositiveStats();
    EXPECT_GE(s.total_checks, static_cast<uint64_t>(reports));
    EXPECT_EQ(s.false_positive_count, static_cast<uint64_t>(reports));
    if (reports > 0) {
        EXPECT_GT(s.runtime_fpr, 0.0);
        EXPECT_LE(s.runtime_fpr, 1.0);
    } else {
        EXPECT_DOUBLE_EQ(s.runtime_fpr, 0.0);
    }
}

INSTANTIATE_TEST_SUITE_P(Reports, BloomOpsMatrixRuntimeFp,
                         ::testing::Values(0, 1, 2, 3, 5, 8, 10, 20, 50, 75, 100,
                                           200, 500));

// -- unready filter is conservatively true across a key matrix ---------------
class BloomOpsMatrixUnready : public ::testing::TestWithParam<std::string> {};

TEST_P(BloomOpsMatrixUnready, AnyKeyIsMaybePresentWhileRebuilding) {
    BloomFilter bf(64 * 1024, 0.01);  // NOT marked ready
    EXPECT_FALSE(bf.IsReady());
    EXPECT_TRUE(bf.MightContain(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Keys, BloomOpsMatrixUnready,
                         ::testing::Values(std::string(""), std::string("a"),
                                           std::string("never-added"),
                                           std::string("fh-XYZ"),
                                           std::string(std::string("k\0z", 3))));

// -- LoadFromCatalog: rebuild over a row-count matrix ------------------------
class BloomOpsMatrixLoad : public ::testing::TestWithParam<int> {};

TEST_P(BloomOpsMatrixLoad, RebuildsCountAndMembership) {
    const int rows = GetParam();
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    ASSERT_EQ(sqlite3_exec(catalog.db(),
        "INSERT INTO tenants(tenant_id, created_at) VALUES('t1',0);"
        "INSERT INTO units(unit_id, tenant_id, created_at) VALUES('u1','t1',0);",
        nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(catalog.db());
    for (int i = 0; i < rows; ++i) {
        std::string sql =
            "INSERT INTO file_locations(file_hash, unit_id, ns_id, tenant_id, "
            "first_seen_at, created_at) VALUES('fh-" + std::to_string(i) +
            "','u1','ns1','t1',0,0);";
        ASSERT_EQ(sqlite3_exec(catalog.db(), sql.c_str(), nullptr, nullptr, nullptr),
                  SQLITE_OK) << sqlite3_errmsg(catalog.db());
    }
    BloomFilter bf(1024 * 1024, 0.01);
    EXPECT_FALSE(bf.IsReady());
    ASSERT_TRUE(bf.LoadFromCatalog(catalog.db()).ok());
    EXPECT_TRUE(bf.IsReady());
    EXPECT_FLOAT_EQ(bf.GetLoadProgress(), 1.0f);
    EXPECT_EQ(bf.EstimatedCount(), static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i)
        EXPECT_TRUE(bf.MightContain("fh-" + std::to_string(i)));
    if (rows > 0) {
        ASSERT_TRUE(bf.LastRebuildAt().has_value());
    }
}

INSTANTIATE_TEST_SUITE_P(Rows, BloomOpsMatrixLoad,
                         ::testing::Values(0, 1, 2, 3, 5, 8, 10, 16, 25, 50, 75,
                                           100, 150, 250, 500));

// -- LoadFromCatalog prepare-failure: file_locations dropped -> not ok -------
TEST(BloomOpsMatrixLoadFailure, MissingFileLocationsTableFails) {
    CatalogDb catalog;
    ASSERT_TRUE(catalog.Open(":memory:").ok());
    // Drop the source table so the rebuild SELECT prepare fails.
    ASSERT_EQ(sqlite3_exec(catalog.db(), "PRAGMA foreign_keys=OFF", nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(catalog.db(), "DROP TABLE file_locations", nullptr, nullptr, nullptr),
              SQLITE_OK) << sqlite3_errmsg(catalog.db());
    BloomFilter bf(64 * 1024, 0.01);
    Status st = bf.LoadFromCatalog(catalog.db());
    EXPECT_FALSE(st.ok());
    // The rebuild did not complete -> filter stays unready and answers true.
    EXPECT_FALSE(bf.IsReady());
    EXPECT_TRUE(bf.MightContain("anything"));
}

// -- LoadFromCatalog null-db arm over a fp-rate matrix -----------------------
class BloomOpsMatrixLoadNull : public ::testing::TestWithParam<double> {};

TEST_P(BloomOpsMatrixLoadNull, NullDbIsInvalidArgument) {
    BloomFilter bf(64 * 1024, GetParam());
    Status st = bf.LoadFromCatalog(nullptr);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
    EXPECT_FALSE(bf.IsReady());
}

INSTANTIATE_TEST_SUITE_P(Rates, BloomOpsMatrixLoadNull,
                         ::testing::Values(0.01, 0.001, 0.5, -1.0, 2.0, 1e-9));

}  // namespace
}  // namespace cortrix::catalog
