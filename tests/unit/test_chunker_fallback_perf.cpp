#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "cortrix/chunker/chunker_errors.h"
#include "cortrix/chunker/parent_child_chunker.h"
#include "cortrix/spc/parser.h"

namespace cortrix::chunker {
namespace {

using cortrix::spc::ParsedChunk;
using cortrix::spc::ParsedPage;

ParsedPage MakePageWithParas(int page_num, int n_paras, int words_each) {
    ParsedPage p;
    p.page_num = page_num;
    for (int i = 0; i < n_paras; ++i) {
        std::string t;
        for (int w = 0; w < words_each; ++w) { if (w) t += " "; t += "tok"; }
        ParsedChunk c; c.text = t; c.page = page_num;
        p.paragraphs.push_back(c);
        if (!p.page_text.empty()) p.page_text += "\n";
        p.page_text += t;
    }
    return p;
}

ChunkerInput MakeInput(std::vector<ParsedPage> pages) {
    ChunkerInput in;
    in.doc_id = "DOC_FB";
    in.namespace_id = "NS1";
    in.pages = std::move(pages);
    return in;
}

// --- S3.2 flat fallback (/ lock) --------------------------------------

TEST(ChunkerFallbackTest, ExplicitFlatStrategyProducesNoParents) {
    ChunkerConfig cfg;
    cfg.strategy = ChunkStrategy::kFlat;
    cfg.parent_size = 256;
    cfg.child_size = 50;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePageWithParas(1, 4, 60)}));
    ASSERT_TRUE(r.ok()) << r.status().message();
    const auto& out = r.value();
    EXPECT_TRUE(out.parents.empty());          // flat: no parents
    EXPECT_FALSE(out.children.empty());
    EXPECT_TRUE(out.stats.fallback_to_flat);
    // every flat child has no parent
    for (const auto& c : out.children) EXPECT_TRUE(c.parent_id.empty());
}

TEST(ChunkerFallbackTest, ThresholdTriggersFallback) {
    // Tiny threshold + many tokens -> estimated parents exceed threshold -> flat.
    ChunkerConfig cfg;
    cfg.parent_size = 256;             // est_parents = total_tokens / 256
    cfg.child_size = 50;
    cfg.max_parents_per_doc = 2;
    cfg.fallback_to_flat_threshold = 2;  //: == cap
    // ~10 paragraphs * 100 tokens = ~1000 tokens -> est ~4 parents > 2 -> fallback
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePageWithParas(1, 10, 100)}));
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(r.value().stats.fallback_to_flat);
    EXPECT_TRUE(r.value().parents.empty());
    EXPECT_FALSE(r.value().children.empty());
}

TEST(ChunkerFallbackTest, BelowThresholdStaysParentChild) {
    ChunkerConfig cfg;
    cfg.parent_size = 256;
    cfg.child_size = 50;
    cfg.max_parents_per_doc = 10000;
    cfg.fallback_to_flat_threshold = 10000;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePageWithParas(1, 4, 60)}));
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.value().stats.fallback_to_flat);
    EXPECT_FALSE(r.value().parents.empty());
}

TEST(ChunkerFallbackTest, OverflowWhenFallbackDisabledBelowCap) {
    // strategy=parent-child, fallback threshold below cap but we still build the
    // tree; if realized parents > cap and we did NOT take the flat path, it's a
    // hard CX_ERR_CHUNK_OVERFLOW. We engineer that by setting fallback threshold
    // HIGH (so no fallback) but cap LOW (so the realized count exceeds it).
    ChunkerConfig cfg;
    cfg.parent_size = 50;                 // many small parents
    cfg.child_size = 30;
    cfg.max_parents_per_doc = 2;          // low cap
    cfg.fallback_to_flat_threshold = 100000;  // high -> never fallback
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePageWithParas(1, 10, 40)}));
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find(chunk_errors::kOverflow), std::string::npos);
}

TEST(ChunkerFallbackTest, FlatChildrenChunkIndexSequential) {
    ChunkerConfig cfg;
    cfg.strategy = ChunkStrategy::kFlat;
    cfg.child_size = 40;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePageWithParas(1, 5, 80)}));
    ASSERT_TRUE(r.ok());
    const auto& kids = r.value().children;
    ASSERT_GE(kids.size(), 2u);
    for (size_t i = 0; i < kids.size(); ++i) EXPECT_EQ(kids[i].chunk_index, static_cast<uint32_t>(i));
}

// --- S3.4 perf-style (L1: chunking throughput) --------------------------

TEST(ChunkerPerfTest, LargeDocChunksQuickly) {
    // Perf case: a large document chunks well under the L1 budget. Build a
    // ~200-page doc with substantial text and assert it completes fast + produces a
    // coherent parent/child tree. (Wall-clock is generous to avoid CI flakiness;
    // the real D-perf recall gate vs benchmark dataset is/)
    std::vector<ParsedPage> pages;
    for (int p = 1; p <= 200; ++p) pages.push_back(MakePageWithParas(p, 8, 120));

    auto t0 = std::chrono::steady_clock::now();
    auto r = ParentChildChunker().Chunk(MakeInput(std::move(pages)));
    auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(r.ok()) << r.status().message();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    EXPECT_LT(ms, 5000.0) << "chunking took " << ms << "ms";  // generous L1 ceiling
    EXPECT_GT(r.value().parents.size(), 0u);
    EXPECT_GT(r.value().children.size(), r.value().parents.size());  // children >= parents
    EXPECT_EQ(r.value().stats.total_pages, 200u);
    EXPECT_EQ(r.value().stats.succeeded_pages, 200u);
}

TEST(ChunkerPerfTest, ChildrenCoverParentText) {
    // Sanity invariant: concatenated child token counts roughly cover the parent
    // (with overlap they may exceed). Guards against a splitter that drops content.
    ChunkerConfig cfg;
    cfg.parent_size = 400;
    cfg.child_size = 100;
    cfg.child_overlap = 20;
    auto r = ParentChildChunker(cfg).Chunk(MakeInput({MakePageWithParas(1, 1, 380)}));
    ASSERT_TRUE(r.ok());
    ASSERT_FALSE(r.value().parents.empty());
    const auto& parent = r.value().parents[0];
    uint32_t child_tokens = 0;
    int n = 0;
    for (const auto& c : r.value().children) {
        if (c.parent_id == parent.parent_id) { child_tokens += c.token_count; ++n; }
    }
    ASSERT_GT(n, 0);
    // children should cover at least ~70% of the parent token budget (overlap may
    // push the sum above 100%).
    EXPECT_GE(child_tokens, parent.token_count * 7 / 10);
}

}  // namespace
}  // namespace cortrix::chunker
