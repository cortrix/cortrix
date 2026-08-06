#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "cortrix/retrieval/splade_sparse_retriever.h"

// Sparse retrieval S6 — SpladeSparseRetriever query: score accumulator (Σ q·c over
// shared terms), top-K truncation, per-term posting-list cap, and the posting-
// list LRU cache (hit-rate / invalidation). In-memory DB; algorithm-level.
namespace cortrix::retrieval {
namespace {

SparseVector V(std::map<uint32_t, float> t) {
    SparseVector v;
    v.terms = std::move(t);
    return v;
}

std::unique_ptr<SpladeSparseRetriever> Open(SpladeConfig cfg = {}) {
    auto r = std::make_unique<SpladeSparseRetriever>(cfg, ":memory:");
    EXPECT_TRUE(r->Open().ok());
    return r;
}

// ---------- score accumulation ----------

TEST(SparseSpladeQueryTest, SingleTermScoreIsProduct) {
    auto r = Open();
    r->Add("ns", "c1", V({{1, 0.8f}}));
    auto hits = r->Search(V({{1, 0.5f}}), "ns", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FLOAT_EQ(hits[0].score, 0.5f * 0.8f);  // 0.40
}

TEST(SparseSpladeQueryTest, MultiTermScoreAccumulates) {
    auto r = Open();
    // c1 shares terms 1 and 2 with the query; term 3 is c1-only (not in query).
    r->Add("ns", "c1", V({{1, 0.6f}, {2, 0.4f}, {3, 0.9f}}));
    auto hits = r->Search(V({{1, 0.5f}, {2, 0.5f}}), "ns", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_FLOAT_EQ(hits[0].score, 0.5f * 0.6f + 0.5f * 0.4f);  // 0.50
}

TEST(SparseSpladeQueryTest, NoSharedTermsNoHit) {
    auto r = Open();
    r->Add("ns", "c1", V({{10, 0.9f}}));
    auto hits = r->Search(V({{1, 1.0f}}), "ns", 10);  // disjoint terms
    EXPECT_TRUE(hits.empty());
}

TEST(SparseSpladeQueryTest, RanksMultipleChildren) {
    auto r = Open();
    r->Add("ns", "low",  V({{1, 0.1f}}));
    r->Add("ns", "high", V({{1, 0.9f}}));
    r->Add("ns", "mid",  V({{1, 0.5f}}));
    auto hits = r->Search(V({{1, 1.0f}}), "ns", 10);
    ASSERT_EQ(hits.size(), 3u);
    EXPECT_EQ(hits[0].child_id, "high");
    EXPECT_EQ(hits[1].child_id, "mid");
    EXPECT_EQ(hits[2].child_id, "low");
}

TEST(SparseSpladeQueryTest, ChildScoredAcrossMultiplePostings) {
    // A child appearing in several query terms' posting lists accumulates.
    auto r = Open();
    r->Add("ns", "x", V({{1, 0.3f}, {2, 0.3f}, {3, 0.3f}}));
    r->Add("ns", "y", V({{1, 0.8f}}));  // strong on one term only
    auto hits = r->Search(V({{1, 1.0f}, {2, 1.0f}, {3, 1.0f}}), "ns", 10);
    ASSERT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].child_id, "x");           // 0.9 > 0.8
    EXPECT_FLOAT_EQ(hits[0].score, 0.9f);
    EXPECT_FLOAT_EQ(hits[1].score, 0.8f);
}

// ---------- top-K ----------

TEST(SparseSpladeQueryTest, TopKTruncates) {
    auto r = Open();
    for (int i = 0; i < 10; ++i) {
        r->Add("ns", "c" + std::to_string(i), V({{1, 0.1f * (i + 1)}}));
    }
    auto hits = r->Search(V({{1, 1.0f}}), "ns", /*top_k=*/3);
    ASSERT_EQ(hits.size(), 3u);
    // Highest 3 weights → c9, c8, c7.
    EXPECT_EQ(hits[0].child_id, "c9");
    EXPECT_EQ(hits[2].child_id, "c7");
}

TEST(SparseSpladeQueryTest, TopKZeroOrNegativeUsesDefault) {
    SpladeConfig cfg;
    cfg.default_top_k = 2;
    auto r = Open(cfg);
    for (int i = 0; i < 5; ++i) {
        r->Add("ns", "c" + std::to_string(i), V({{1, 0.1f * (i + 1)}}));
    }
    EXPECT_EQ(r->Search(V({{1, 1.0f}}), "ns", 0).size(), 2u);   // 0 → default
    EXPECT_EQ(r->Search(V({{1, 1.0f}}), "ns", -1).size(), 2u);  // <0 → default
}

TEST(SparseSpladeQueryTest, EmptyQueryReturnsEmpty) {
    auto r = Open();
    r->Add("ns", "c", V({{1, 0.5f}}));
    EXPECT_TRUE(r->Search(V({}), "ns", 10).empty());
}

TEST(SparseSpladeQueryTest, NotOpenSearchFlagsFallback) {
    SpladeSparseRetriever r(SpladeConfig{}, ":memory:");  // no Open()
    auto hits = r.Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_TRUE(hits.empty());
    EXPECT_TRUE(r.last_search_failed());  // L2 fallback signal
}

TEST(SparseSpladeQueryTest, OpenSearchDoesNotFlagFallback) {
    auto r = Open();
    r->Add("ns", "c", V({{1, 0.5f}}));
    (void)r->Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_FALSE(r->last_search_failed());
}

// ---------- per-term posting-list cap ----------

TEST(SparseSpladeQueryTest, PostingListCapLimitsScan) {
    SpladeConfig cfg;
    cfg.posting_list_cap = 5;  // only top-5 by weight per term are scanned
    auto r = Open(cfg);
    // 20 children all share term 1 with increasing weights.
    for (int i = 0; i < 20; ++i) {
        r->Add("ns", "c" + std::to_string(i), V({{1, 0.01f * (i + 1)}}));
    }
    auto hits = r->Search(V({{1, 1.0f}}), "ns", 100);
    // Cap means only the 5 highest-weight postings are visible.
    EXPECT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0].child_id, "c19");  // highest weight survives the cap
}

// ---------- posting-list LRU cache ----------

TEST(SparseSpladeQueryTest, RepeatedQueryHitsCache) {
    auto r = Open();
    r->Add("ns", "c", V({{1, 0.5f}, {2, 0.5f}}));
    // 1st query: 2 term lookups → 2 misses. Repeat 9× → 18 hits.
    for (int i = 0; i < 10; ++i) {
        (void)r->Search(V({{1, 1.0f}, {2, 1.0f}}), "ns", 10);
    }
    // 18 hits / 20 lookups = 0.9 ≥ 0.70 target.
    EXPECT_GE(r->cache_hit_rate(), 0.70);
}

TEST(SparseSpladeQueryTest, AddInvalidatesCachedPostingList) {
    auto r = Open();
    r->Add("ns", "c1", V({{1, 0.5f}}));
    ASSERT_EQ(r->Search(V({{1, 1.0f}}), "ns", 10).size(), 1u);  // caches term 1
    // A new child on term 1 must appear → cache for term 1 invalidated by Add.
    r->Add("ns", "c2", V({{1, 0.8f}}));
    auto hits = r->Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_EQ(hits.size(), 2u);
    EXPECT_EQ(hits[0].child_id, "c2");  // freshly indexed, higher weight
}

TEST(SparseSpladeQueryTest, RemoveInvalidatesCachedPostingList) {
    auto r = Open();
    r->Add("ns", "c1", V({{1, 0.5f}}));
    r->Add("ns", "c2", V({{1, 0.8f}}));
    ASSERT_EQ(r->Search(V({{1, 1.0f}}), "ns", 10).size(), 2u);  // caches term 1
    r->Remove("ns", "c2");
    auto hits = r->Search(V({{1, 1.0f}}), "ns", 10);
    EXPECT_EQ(hits.size(), 1u);          // c2 gone from results
    EXPECT_EQ(hits[0].child_id, "c1");
}

TEST(SparseSpladeQueryTest, CacheEvictsBeyondCapacity) {
    SpladeConfig cfg;
    cfg.posting_cache_capacity = 2;  // tiny LRU
    auto r = Open(cfg);
    // Index 5 distinct terms (each its own posting list / cache entry).
    for (uint32_t t = 1; t <= 5; ++t) {
        r->Add("ns", "c" + std::to_string(t), V({{t, 0.5f}}));
    }
    // Query each term once — cache holds at most 2 entries, so most are misses.
    for (uint32_t t = 1; t <= 5; ++t) {
        (void)r->Search(V({{t, 1.0f}}), "ns", 10);
    }
    // Each query still returns its child (eviction is transparent / correct).
    for (uint32_t t = 1; t <= 5; ++t) {
        auto hits = r->Search(V({{t, 1.0f}}), "ns", 10);
        ASSERT_EQ(hits.size(), 1u) << "term " << t;
        EXPECT_EQ(hits[0].child_id, "c" + std::to_string(t));
    }
}

}  // namespace
}  // namespace cortrix::retrieval
