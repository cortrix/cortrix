#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/query/cross_ns_dedup.h"

// S3.3 coverage: DedupeByContentHash B-simplified — hash-table dedup, primary = highest
// final score source, meta.deduplicated_chunks[] abbreviated multi-source, deduplicated_chunks_count.
namespace cortrix::query {
namespace {

using retrieval::ResultItem;

ResultItem Item(const std::string& ns, const std::string& hash, float rerank,
                const std::string& child = "") {
    ResultItem it;
    it.namespace_id = ns;
    it.content_hash = hash;
    it.rerank_score = rerank;
    it.score = rerank;
    it.child_id = child.empty() ? (ns + "_" + hash) : child;
    it.content = ns + " content";
    it.metadata["src"] = ns;  // distinct metadata to prove primary's is kept
    return it;
}

// No duplicates → list unchanged, nothing recorded.
TEST(CrossNsDedupTest, NoDuplicatesPassThrough) {
    std::vector<ResultItem> items = {Item("ns_a", "sha256:aaa", 0.9f),
                                     Item("ns_b", "sha256:bbb", 0.8f)};
    CrossNsMeta meta;
    int collapsed = DedupeByContentHash(items, meta);

    EXPECT_EQ(collapsed, 0);
    EXPECT_EQ(items.size(), 2u);
    EXPECT_TRUE(meta.deduplicated_chunks.empty());
    EXPECT_EQ(meta.deduplicated_chunks_count, 0);
}

// Two NS share one content_hash → collapse to the higher final-score primary.
TEST(CrossNsDedupTest, CollapsesDuplicateAcrossNamespaces) {
    std::vector<ResultItem> items = {
        Item("ns_low", "sha256:dup", 0.70f),
        Item("ns_high", "sha256:dup", 0.95f),
    };
    CrossNsMeta meta;
    int collapsed = DedupeByContentHash(items, meta);

    EXPECT_EQ(collapsed, 1);                      // one copy removed
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].namespace_id, "ns_high");  // primary = highest final score
    EXPECT_EQ(items[0].metadata.at("src"), "ns_high");  // primary's full metadata kept

    ASSERT_EQ(meta.deduplicated_chunks.size(), 1u);
    const auto& d = meta.deduplicated_chunks[0];
    EXPECT_EQ(d.content_hash, "sha256:dup");
    EXPECT_EQ(d.primary_namespace, "ns_high");
    ASSERT_EQ(d.namespaces.size(), 2u);           // abbreviated multi-source: both sources listed
    EXPECT_EQ(meta.deduplicated_chunks_count, 1);
}

// The abbreviated multi-source array carries each source's (NS, final score, rerank_score).
TEST(CrossNsDedupTest, RecordsAllSourcesWithScores) {
    std::vector<ResultItem> items = {
        Item("ns_a", "sha256:x", 0.6f),
        Item("ns_b", "sha256:x", 0.9f),
        Item("ns_c", "sha256:x", 0.7f),
    };
    CrossNsMeta meta;
    int collapsed = DedupeByContentHash(items, meta);

    EXPECT_EQ(collapsed, 2);                       // 3 copies → 1 primary, 2 removed
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].namespace_id, "ns_b");      // 0.9 wins
    ASSERT_EQ(meta.deduplicated_chunks.size(), 1u);
    EXPECT_EQ(meta.deduplicated_chunks[0].namespaces.size(), 3u);
    EXPECT_EQ(meta.deduplicated_chunks_count, 2);
}

TEST(CrossNsDedupTest, FinalScoreWinsOverRawRerankScore) {
    auto high_rerank_low_final = Item("ns_high_rerank", "sha256:x", 0.95f);
    high_rerank_low_final.score = 0.40f;
    auto low_rerank_high_final = Item("ns_high_final", "sha256:x", 0.50f);
    low_rerank_high_final.score = 0.90f;
    std::vector<ResultItem> items = {high_rerank_low_final, low_rerank_high_final};

    CrossNsMeta meta;
    int collapsed = DedupeByContentHash(items, meta);

    EXPECT_EQ(collapsed, 1);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].namespace_id, "ns_high_final");
    ASSERT_EQ(meta.deduplicated_chunks.size(), 1u);
    ASSERT_EQ(meta.deduplicated_chunks[0].namespaces.size(), 2u);
    EXPECT_FLOAT_EQ(meta.deduplicated_chunks[0].namespaces[0].score, 0.40f);
    EXPECT_FLOAT_EQ(meta.deduplicated_chunks[0].namespaces[0].rerank_score, 0.95f);
}

// Distinct hashes are independent; only the colliding ones collapse.
TEST(CrossNsDedupTest, MixedDuplicateAndUnique) {
    std::vector<ResultItem> items = {
        Item("ns_a", "sha256:dup", 0.9f),
        Item("ns_b", "sha256:uniq", 0.8f),
        Item("ns_c", "sha256:dup", 0.5f),
    };
    CrossNsMeta meta;
    int collapsed = DedupeByContentHash(items, meta);

    EXPECT_EQ(collapsed, 1);
    ASSERT_EQ(items.size(), 2u);                   // dup→1 + uniq
    EXPECT_EQ(meta.deduplicated_chunks.size(), 1u);
    EXPECT_EQ(meta.deduplicated_chunks_count, 1);
}

// First-appearance order is preserved (a pre-sorted-by-final-score input stays sorted).
TEST(CrossNsDedupTest, PreservesFirstAppearanceOrder) {
    std::vector<ResultItem> items = {
        Item("ns_a", "sha256:hi", 0.9f),   // distinct, highest
        Item("ns_b", "sha256:mid", 0.8f),  // collides with ns_d below
        Item("ns_c", "sha256:lo", 0.3f),   // distinct, lowest
        Item("ns_d", "sha256:mid", 0.5f),  // lower copy of mid
    };
    CrossNsMeta meta;
    DedupeByContentHash(items, meta);

    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].content_hash, "sha256:hi");
    EXPECT_EQ(items[1].content_hash, "sha256:mid");  // primary ns_b (0.8 > 0.5)
    EXPECT_EQ(items[1].namespace_id, "ns_b");
    EXPECT_EQ(items[2].content_hash, "sha256:lo");
}

// Empty content_hash items are never merged (a missing hash ≠ a match).
TEST(CrossNsDedupTest, EmptyHashNeverCollapses) {
    std::vector<ResultItem> items = {Item("ns_a", "", 0.9f),
                                     Item("ns_b", "", 0.8f)};
    CrossNsMeta meta;
    int collapsed = DedupeByContentHash(items, meta);

    EXPECT_EQ(collapsed, 0);
    EXPECT_EQ(items.size(), 2u);                   // both kept, distinct
    EXPECT_TRUE(meta.deduplicated_chunks.empty());
}

// Tie on final score → earliest occurrence is primary (stable).
TEST(CrossNsDedupTest, TieResolvesToEarliest) {
    std::vector<ResultItem> items = {Item("ns_first", "sha256:t", 0.8f),
                                     Item("ns_second", "sha256:t", 0.8f)};
    CrossNsMeta meta;
    DedupeByContentHash(items, meta);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].namespace_id, "ns_first");
}

// Empty input is a no-op.
TEST(CrossNsDedupTest, EmptyInput) {
    std::vector<ResultItem> items;
    CrossNsMeta meta;
    EXPECT_EQ(DedupeByContentHash(items, meta), 0);
    EXPECT_TRUE(items.empty());
}

}  // namespace
}  // namespace cortrix::query
