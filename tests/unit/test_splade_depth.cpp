#include <gtest/gtest.h>

#include <map>
#include <memory>

#include "cortrix/retrieval/splade_sparse_retriever.h"

// DEPTH — SpladeSparseRetriever gaps the coverage mapping flagged:
//   1. Close()-then-Search flips last_search_failed() (distinct from the existing
//      "never Open()ed" test: this exercises the Close() path nulling db_ AFTER a
//      successful Open + writes, the realistic L2-fallback-after-shutdown arm).
//   2. cache_hit_rate() EXACT value (the existing test only asserts >= 0.70).
// Suite name is globally unique (SpladeDepth).
namespace cortrix::retrieval {
namespace {

SparseVector V(std::map<uint32_t, float> t) {
    SparseVector v;
    v.terms = std::move(t);
    return v;
}

std::unique_ptr<SpladeSparseRetriever> OpenRetriever(SpladeConfig cfg = {}) {
    auto r = std::make_unique<SpladeSparseRetriever>(cfg, ":memory:");
    EXPECT_TRUE(r->Open().ok());
    return r;
}

// --- 1. Close()-then-Search flips last_search_failed ---

TEST(SpladeDepthTest, CloseThenSearchFlagsFallback) {
    auto r = OpenRetriever();
    r->Add("ns", "c1", V({{1, 0.8f}}));
    // A search before Close succeeds (no fallback flag).
    auto pre = r->Search(V({{1, 1.0f}}), "ns", 10);
    ASSERT_FALSE(r->last_search_failed());
    ASSERT_EQ(pre.size(), 1u);

    r->Close();  // db_ -> nullptr
    EXPECT_FALSE(r->IsAvailable());

    // Search after Close: empty result + last_search_failed() flipped (the
    // L2 fallback signal), and never throws.
    auto post = r->Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_TRUE(post.empty());
    EXPECT_TRUE(r->last_search_failed());
}

TEST(SpladeDepthTest, SearchAfterCloseResetsFlagOnEachCall) {
    auto r = OpenRetriever();
    r->Add("ns", "c1", V({{1, 0.5f}}));
    r->Close();
    // Two consecutive post-close searches both flag (flag is reset at entry then
    // re-set by the null-db guard).
    (void)r->Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_TRUE(r->last_search_failed());
    (void)r->Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_TRUE(r->last_search_failed());
}

// --- 2. cache_hit_rate exact value ---

TEST(SpladeDepthTest, CacheHitRateZeroBeforeAnySearch) {
    auto r = OpenRetriever();
    r->Add("ns", "c1", V({{1, 0.5f}}));
    // No Search() yet → no posting-list reads → 0/0 → 0.0 by definition.
    EXPECT_DOUBLE_EQ(r->cache_hit_rate(), 0.0);
}

TEST(SpladeDepthTest, CacheHitRateExactHalfAfterMissThenHit) {
    auto r = OpenRetriever();
    r->Add("ns", "c1", V({{7, 0.5f}}));
    // First search of term 7: cold cache → 1 miss. Second identical search: warm
    // → 1 hit. Total 1 hit / 2 lookups → exactly 0.5.
    (void)r->Search(V({{7, 1.0f}}), "ns", 10);
    (void)r->Search(V({{7, 1.0f}}), "ns", 10);
    EXPECT_DOUBLE_EQ(r->cache_hit_rate(), 0.5);
}

TEST(SpladeDepthTest, CacheHitRateExactTwoThirds) {
    auto r = OpenRetriever();
    r->Add("ns", "c1", V({{9, 0.5f}}));
    // 1 miss + 2 hits over three identical single-term searches → 2/3.
    (void)r->Search(V({{9, 1.0f}}), "ns", 10);  // miss
    (void)r->Search(V({{9, 1.0f}}), "ns", 10);  // hit
    (void)r->Search(V({{9, 1.0f}}), "ns", 10);  // hit
    EXPECT_NEAR(r->cache_hit_rate(), 2.0 / 3.0, 1e-9);
}

TEST(SpladeDepthTest, CacheHitRateAllMissesIsZero) {
    auto r = OpenRetriever();
    r->Add("ns", "c1", V({{1, 0.5f}}));
    // Each search uses a DISTINCT term → every lookup is a cold miss → 0/N == 0.0.
    (void)r->Search(V({{1, 1.0f}}), "ns", 10);
    (void)r->Search(V({{2, 1.0f}}), "ns", 10);
    (void)r->Search(V({{3, 1.0f}}), "ns", 10);
    EXPECT_DOUBLE_EQ(r->cache_hit_rate(), 0.0);
}

}  // namespace
}  // namespace cortrix::retrieval
