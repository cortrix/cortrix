#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>

#include "cortrix/retrieval/sparse_fallback.h"

// Sparse retrieval S9 — L1 (write-time) + L2 (query-time) fallback. L1: serialize with
// N=3 retry then degrade to NULL (no chunk-write failure). L2: drop the sparse
// path, transparent degrade to dense+FTS5(+hype), via_path explain.
namespace cortrix::retrieval {
namespace {

SparseVector V(std::map<uint32_t, float> t) {
    SparseVector v;
    v.terms = std::move(t);
    return v;
}
std::vector<SparseHit> L(std::vector<std::string> ids) {
    std::vector<SparseHit> v;
    float s = 1.0f;
    for (auto& id : ids) v.push_back({id, s--});
    return v;
}

// ---------- L1 write-time ----------

TEST(SparseFallbackTest, L1SerializesNonEmptyAndSetsFlag) {
    auto r = L1SerializeSparseVec(V({{1, 0.5f}, {2, 0.3f}}));
    EXPECT_TRUE(r.serialized);
    EXPECT_TRUE(r.set_has_sparse_vec);
    EXPECT_FALSE(r.blob.empty());
    EXPECT_EQ(r.attempts, 1);  // succeeds first try
}

TEST(SparseFallbackTest, L1EmptyVectorWritesNullClearsFlagNoError) {
    auto r = L1SerializeSparseVec(V({}));  // dead chunk
    EXPECT_FALSE(r.serialized);            // → write NULL
    EXPECT_FALSE(r.set_has_sparse_vec);    // flag cleared
    EXPECT_TRUE(r.blob.empty());
    EXPECT_EQ(r.attempts, 0);              // no serialize attempt for empty
}

TEST(SparseFallbackTest, L1OutOfRangeIdDegradesAfterRetries) {
    // term_id > 65535 makes SerializeSparseVec fail deterministically → after
    // N=3 retries (4 attempts total) it degrades to NULL, NOT a chunk error.
    auto r = L1SerializeSparseVec(V({{70000u, 0.9f}}), /*retries=*/3);
    EXPECT_FALSE(r.serialized);
    EXPECT_FALSE(r.set_has_sparse_vec);
    EXPECT_EQ(r.attempts, 4);  // initial + 3 retries
}

TEST(SparseFallbackTest, L1RetriesCountConfigurable) {
    auto r0 = L1SerializeSparseVec(V({{70000u, 0.9f}}), /*retries=*/0);
    EXPECT_FALSE(r0.serialized);
    EXPECT_EQ(r0.attempts, 1);  // just the initial attempt
}

TEST(SparseFallbackTest, L1RoundTripBlobIsValid) {
    auto r = L1SerializeSparseVec(V({{5, 0.7f}, {9, 0.2f}}));
    ASSERT_TRUE(r.serialized);
    auto back = DeserializeSparseVec(r.blob);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().terms.size(), 2u);
}

// ---------- L2 query-time ----------

TEST(SparseFallbackTest, L2NormalPathWhenAvailable) {
    auto ex = DecideL2Fallback(/*sparse_available=*/true, /*resolved_top_k=*/100);
    EXPECT_EQ(ex.via_path, "sparse");
    EXPECT_FALSE(ex.fallback_used);
    EXPECT_EQ(ex.sparse_top_k, 100);
    EXPECT_NE(std::find(ex.fallback_paths.begin(), ex.fallback_paths.end(),
                        "sparse"),
              ex.fallback_paths.end());
}

TEST(SparseFallbackTest, L2FallbackWhenUnavailable) {
    auto ex = DecideL2Fallback(/*sparse_available=*/false, /*resolved_top_k=*/120);
    EXPECT_EQ(ex.via_path, "fallback_dense_fts5_hype");
    EXPECT_TRUE(ex.fallback_used);
    EXPECT_EQ(ex.sparse_top_k, 120);
    // sparse must NOT be among the fallback paths.
    EXPECT_EQ(std::find(ex.fallback_paths.begin(), ex.fallback_paths.end(),
                        "sparse"),
              ex.fallback_paths.end());
}

TEST(SparseFallbackTest, DropSparsePathZeroesSparseOnly) {
    FivePathInput in;
    in.dense = L({"a", "b"});
    in.sparse = L({"a"});
    in.fts5 = L({"b"});
    auto dropped = DropSparsePath(in);
    EXPECT_TRUE(dropped.sparse.empty());     // sparse gone
    EXPECT_EQ(dropped.dense.size(), 2u);     // others preserved
    EXPECT_EQ(dropped.fts5.size(), 1u);
}

TEST(SparseFallbackTest, L2FusionDegradesToFourPath) {
    // Build a normal 5-path input, then drop sparse → RRF must still fuse the
    // remaining paths (4-path degrade), and a sparse-only child disappears.
    FivePathInput in;
    in.dense = L({"a"});
    in.sparse = L({"sparse_only"});  // only reachable via sparse
    in.fts5 = L({"a"});

    auto normal = FuseFivePathRrf(in);
    // "sparse_only" present on the normal path.
    bool has_sparse_only = std::any_of(
        normal.begin(), normal.end(),
        [](const RrfFusedHit& h) { return h.child_id == "sparse_only"; });
    EXPECT_TRUE(has_sparse_only);

    auto degraded = FuseFivePathRrf(DropSparsePath(in));
    bool still_there = std::any_of(
        degraded.begin(), degraded.end(),
        [](const RrfFusedHit& h) { return h.child_id == "sparse_only"; });
    EXPECT_FALSE(still_there);  // dropped with the sparse path
    // "a" survives via dense + fts5.
    bool has_a = std::any_of(degraded.begin(), degraded.end(),
                             [](const RrfFusedHit& h) { return h.child_id == "a"; });
    EXPECT_TRUE(has_a);
}

}  // namespace
}  // namespace cortrix::retrieval
