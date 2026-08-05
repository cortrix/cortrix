// S1.1 — IReranker abstract interface + retrieval types compile/instantiation.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cortrix/reranker.h"
#include "cortrix/retrieval/types.h"
#include "mock_reranker.h"

namespace cortrix::reranker {
namespace {

using ::testing::_;
using ::testing::Return;
using retrieval::RankedChunk;
using retrieval::ScoredResult;

// --- retrieval/types.h (the header F02 introduces) ---

TEST(RetrievalTypesTest, ScoredResultHoldsChildIdAndScore) {
    ScoredResult r{"01J0CHILDAAAAAAAAAAAAAAAAA", 0.42f};
    EXPECT_EQ(r.child_id, "01J0CHILDAAAAAAAAAAAAAAAAA");
    EXPECT_FLOAT_EQ(r.score, 0.42f);
}

TEST(RetrievalTypesTest, RankedChunkCarriesFullContentFields) {
    RankedChunk rc;
    rc.child_id = "01J0CHILDBBBBBBBBBBBBBBBBB";
    rc.chunk_text = "hello world";
    rc.parent_text = "the parent paragraph";
    rc.score = 0.1f;          // pre-rerank (RRF) score
    rc.rerank_score = 0.9f;   // cross-encoder score
    rc.metadata["source"] = "doc_1";

    EXPECT_EQ(rc.chunk_text, "hello world");
    EXPECT_EQ(rc.parent_text, "the parent paragraph");
    EXPECT_FLOAT_EQ(rc.score, 0.1f);
    EXPECT_FLOAT_EQ(rc.rerank_score, 0.9f);
    EXPECT_EQ(rc.metadata.at("source"), "doc_1");
}

// --- IReranker interface: a concrete subclass is instantiable through the
// interface pointer (S1.1 DoD "subclass inheritance is instantiable"). ---

class StubReranker : public IReranker {
public:
    float Score(const char*, const char*) override { return 1.0f; }
    std::vector<float> ScoreBatch(const char*,
                                  const std::vector<const char*>& p) override {
        return std::vector<float>(p.size(), 1.0f);
    }
    std::vector<RankedChunk> Rerank(const std::vector<ScoredResult>& c,
                                    const std::string&) override {
        std::vector<RankedChunk> out;
        for (const auto& s : c) {
            out.push_back(RankedChunk{s.child_id, "", "", s.score, 1.0f, {}, {}});
        }
        return out;
    }
    const char* Name() const override { return "StubReranker"; }
};

TEST(IRerankerTest, ConcreteSubclassUsableThroughInterfacePointer) {
    StubReranker stub;
    IReranker* r = &stub;  // downstream holds only the interface
    EXPECT_STREQ(r->Name(), "StubReranker");
    EXPECT_FLOAT_EQ(r->Score("q", "p"), 1.0f);

    std::vector<const char*> passages{"a", "b", "c"};
    auto scores = r->ScoreBatch("q", passages);
    EXPECT_EQ(scores.size(), 3u);

    std::vector<ScoredResult> cands{{"01CHILD1", 0.5f}, {"01CHILD2", 0.3f}};
    auto ranked = r->Rerank(cands, "q");
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0].child_id, "01CHILD1");
    EXPECT_FLOAT_EQ(ranked[0].score, 0.5f);  // pre-rerank score carried through
}

// --- MockReranker is instantiable and its expectations fire. ---

TEST(MockRerankerTest, GmockDoubleHonorsExpectations) {
    MockReranker mock;
    EXPECT_CALL(mock, Name()).WillRepeatedly(Return("MockReranker"));
    EXPECT_CALL(mock, Score(_, _)).WillOnce(Return(0.77f));
    EXPECT_CALL(mock, ScoreBatch(_, _))
        .WillOnce(Return(std::vector<float>{0.1f, 0.2f}));

    IReranker* r = &mock;
    EXPECT_STREQ(r->Name(), "MockReranker");
    EXPECT_FLOAT_EQ(r->Score("q", "p"), 0.77f);
    std::vector<const char*> passages{"x", "y"};
    EXPECT_EQ(r->ScoreBatch("q", passages).size(), 2u);
}

TEST(MockRerankerTest, RerankExpectationReturnsRankedChunks) {
    MockReranker mock;
    std::vector<RankedChunk> canned{
        RankedChunk{"01CHILDA", "ta", "pa", 0.2f, 0.9f, {}, {}},
        RankedChunk{"01CHILDB", "tb", "pb", 0.1f, 0.8f, {}, {}},
    };
    EXPECT_CALL(mock, Rerank(_, _)).WillOnce(Return(canned));

    std::vector<ScoredResult> cands{{"01CHILDA", 0.2f}, {"01CHILDB", 0.1f}};
    auto out = mock.Rerank(cands, "query");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].child_id, "01CHILDA");
    EXPECT_FLOAT_EQ(out[0].rerank_score, 0.9f);
}

}  // namespace
}  // namespace cortrix::reranker
