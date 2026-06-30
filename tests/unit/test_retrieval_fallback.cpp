#include <gtest/gtest.h>

#include <vector>

#include "cortrix/query/query_context.h"
#include "cortrix/retrieval/retrieval_fallback.h"
#include "cortrix/retrieval/types.h"

// F37 S5 coverage (interface reservation): IRetrievalFallback / NullRetrievalFallback
// (§5.3). Phase 1 the Null impl returns the original chunks unchanged; Phase 2 will
// add WebSearchFallback behind the same interface.
namespace cortrix::retrieval {
namespace {

TEST(RetrievalFallbackTest, NullFallbackReturnsOriginalChunks) {
    NullRetrievalFallback fb;
    EXPECT_EQ(fb.Name(), "null_fallback");

    std::vector<RankedChunk> chunks = {
        RankedChunk{"01A", "ta", "pa", 0.9f, 0.8f, {}, {}},
        RankedChunk{"01B", "tb", "pb", 0.2f, 0.1f, {}, {}},
    };
    query::QueryContext ctx;  // ctx is ignored by the Null impl
    auto out = fb.Fallback(ctx, chunks);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].child_id, "01A");
    EXPECT_EQ(out[1].child_id, "01B");
    EXPECT_FLOAT_EQ(out[0].score, 0.9f);
}

TEST(RetrievalFallbackTest, NullFallbackEmptyInput) {
    NullRetrievalFallback fb;
    query::QueryContext ctx;
    auto out = fb.Fallback(ctx, {});
    EXPECT_TRUE(out.empty());
}

// Polymorphic use through the interface pointer (F37 holds an IRetrievalFallback*).
TEST(RetrievalFallbackTest, UsableThroughInterfacePointer) {
    NullRetrievalFallback impl;
    IRetrievalFallback* fb = &impl;
    std::vector<RankedChunk> chunks = {
        RankedChunk{"01C", "tc", "pc", 0.5f, 0.4f, {}, {}}};
    query::QueryContext ctx;
    auto out = fb->Fallback(ctx, chunks);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].child_id, "01C");
    EXPECT_EQ(fb->Name(), "null_fallback");
}

}  // namespace
}  // namespace cortrix::retrieval
