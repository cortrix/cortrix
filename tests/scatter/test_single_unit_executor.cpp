#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "cortrix/query/i_namespace_pipeline.h"
#include "cortrix/query/single_unit_executor.h"
#include "mock_reranker.h"  // tests/mocks (F02 shared)

// S1.2 coverage: SingleUnitExecutor — candidate over-fetch sizing, rerank path,
// RRF fallback (rerank=false), per-NS index-corrupt in-band error.
namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Return;
using ::testing::Throw;
using reranker::MockReranker;
using retrieval::RankedChunk;
using retrieval::ScoredResult;

// Configurable fake pipeline: records the candidate_k it was asked for, returns a
// preset candidate list (or throws to simulate an internal NS fault).
class FakePipeline : public INamespacePipeline {
public:
    std::vector<ScoredResult> to_return;
    int last_candidate_k = -1;
    bool throw_on_call = false;

    std::vector<ScoredResult> Retrieve(const QueryContext&, const std::string&,
                                       int candidate_k) override {
        last_candidate_k = candidate_k;
        if (throw_on_call) throw std::runtime_error("simulated index corrupt");
        return to_return;
    }
};

QueryContext Ctx(int top_k, bool rerank) {
    QueryContext c;
    c.query = "q";
    c.top_k = top_k;
    c.rerank = rerank;
    return c;
}

// CandidateK = min(top_k × multiplier × ceil(oversample), max_candidates), >= top_k.
TEST(SingleUnitExecutorTest, CandidateKSizing) {
    SingleUnitExecutor exec(nullptr, nullptr, /*multiplier=*/3, /*max=*/50);
    EXPECT_EQ(exec.CandidateK(10, 1.0f), 30);   // 10*3
    EXPECT_EQ(exec.CandidateK(20, 1.0f), 50);   // 20*3=60 capped to 50
    EXPECT_EQ(exec.CandidateK(10, 2.0f), 50);   // 10*3*2=60 capped
    EXPECT_EQ(exec.CandidateK(1, 1.0f), 3);     // 1*3
    EXPECT_EQ(exec.CandidateK(40, 1.0f), 50);   // capped, but >= top_k (40) ok since 50>=40
}

// CandidateK never returns fewer than top_k (cap clamps up to top_k).
TEST(SingleUnitExecutorTest, CandidateKNeverBelowTopK) {
    SingleUnitExecutor exec(nullptr, nullptr, /*multiplier=*/1, /*max=*/5);
    EXPECT_EQ(exec.CandidateK(20, 1.0f), 20);  // 20*1=20 > max 5 → but clamped up to top_k=20
}

// rerank=true: pipeline → IReranker.Rerank → top_k chunks, success (empty error_code).
TEST(SingleUnitExecutorTest, RerankPathReturnsRerankedChunks) {
    FakePipeline pipe;
    pipe.to_return = {{"c0", 0.4f}, {"c1", 0.6f}, {"c2", 0.5f}};

    MockReranker rr;
    std::vector<RankedChunk> reranked = {
        {"c1", "t1", "", 0.6f, 0.95f, {}},
        {"c2", "t2", "", 0.5f, 0.90f, {}},
        {"c0", "t0", "", 0.4f, 0.80f, {}},
    };
    EXPECT_CALL(rr, Rerank(_, _)).WillOnce(Return(reranked));

    SingleUnitExecutor exec(&pipe, &rr);
    auto r = exec.ExecuteForNamespace(Ctx(/*top_k=*/2, /*rerank=*/true), "ns_a");

    EXPECT_EQ(r.namespace_id, "ns_a");
    EXPECT_TRUE(r.error_code.empty());
    ASSERT_EQ(r.chunks.size(), 2u);          // truncated to top_k=2
    EXPECT_EQ(r.chunks[0].child_id, "c1");   // highest rerank_score first
    EXPECT_EQ(r.chunks[1].child_id, "c2");
    EXPECT_EQ(pipe.last_candidate_k, 6);     // over-fetch 2*3
}

// rerank=false: RRF fallback — no reranker call; ordering by RRF score; chunks
// carry child_id + score (chunk_text empty until F08/F09 wiring).
TEST(SingleUnitExecutorTest, RrfFallbackSkipsReranker) {
    FakePipeline pipe;
    pipe.to_return = {{"c0", 0.3f}, {"c1", 0.9f}, {"c2", 0.5f}};

    MockReranker rr;
    EXPECT_CALL(rr, Rerank(_, _)).Times(0);  // fallback must NOT call the reranker

    SingleUnitExecutor exec(&pipe, &rr);
    auto r = exec.ExecuteForNamespace(Ctx(/*top_k=*/2, /*rerank=*/false), "ns_a");

    EXPECT_TRUE(r.error_code.empty());
    ASSERT_EQ(r.chunks.size(), 2u);
    EXPECT_EQ(r.chunks[0].child_id, "c1");  // 0.9 first
    EXPECT_EQ(r.chunks[1].child_id, "c2");  // 0.5 next
    EXPECT_FLOAT_EQ(r.chunks[0].rerank_score, 0.9f);  // RRF score carries ordering
}

// rerank=true but no reranker injected → per-NS CX_ERR_INDEX_CORRUPT (in-band).
TEST(SingleUnitExecutorTest, RerankRequestedWithoutRerankerIsIndexCorrupt) {
    FakePipeline pipe;
    SingleUnitExecutor exec(&pipe, /*reranker=*/nullptr);
    auto r = exec.ExecuteForNamespace(Ctx(2, /*rerank=*/true), "ns_a");
    EXPECT_EQ(r.error_code, "CX_ERR_INDEX_CORRUPT");
    EXPECT_EQ(r.error_category, "permanent");
    EXPECT_FALSE(r.retryable);
    EXPECT_TRUE(r.chunks.empty());
}

// A pipeline fault is surfaced in-band as CX_ERR_INDEX_CORRUPT, NOT thrown
// (Issue 2.4 partial success — ScatterGather keeps other NS's results).
TEST(SingleUnitExecutorTest, PipelineFaultBecomesInBandError) {
    FakePipeline pipe;
    pipe.throw_on_call = true;

    MockReranker rr;
    SingleUnitExecutor exec(&pipe, &rr);
    retrieval::NamespaceQueryResult r;
    ASSERT_NO_THROW(r = exec.ExecuteForNamespace(Ctx(2, true), "ns_bad"));
    EXPECT_EQ(r.namespace_id, "ns_bad");
    EXPECT_EQ(r.error_code, "CX_ERR_INDEX_CORRUPT");
    EXPECT_TRUE(r.chunks.empty());
}

// Empty candidate set → success with zero chunks (Issue 2.4 empty result).
TEST(SingleUnitExecutorTest, EmptyCandidatesIsSuccessZeroChunks) {
    FakePipeline pipe;  // to_return empty
    MockReranker rr;
    EXPECT_CALL(rr, Rerank(_, _)).WillOnce(Return(std::vector<RankedChunk>{}));
    SingleUnitExecutor exec(&pipe, &rr);
    auto r = exec.ExecuteForNamespace(Ctx(5, true), "ns_a");
    EXPECT_TRUE(r.error_code.empty());
    EXPECT_TRUE(r.chunks.empty());
}

}  // namespace
}  // namespace cortrix::query
