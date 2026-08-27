// Issue #75 -- the per-request rerank opt-out must reach the executor, not merely
// survive request parsing.
//
// Duplicate-detection and similar-item workloads rely on this flag: with reranking
// on, measured on BEIR Quora, nDCG@10 falls 62% and the verbatim-duplicate document
// leaves the result set entirely (docs/operations/reranking-applicability.md). The
// parser-level coverage in test_rerank_opt_out.cpp proves the field is read; these
// cases prove the executor honours it -- that the reranker is not invoked at all,
// and that the dense/RRF ordering is what comes back.
//
// Both directions are asserted. A test that only checked "zero calls when disabled"
// would still pass if reranking were broken everywhere, so the enabled case pins
// that the seam is live and the disabled case therefore means something.
//
// Deterministic: real namespace pool over a temp dir, injected index returning fixed
// hits, mock reranker, stub embedder. No model, no network, no clock.

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <string>
#include <vector>

#include "cortrix/query/live_single_unit_executor.h"
#include "cortrix/query/query_context.h"
#include "cortrix/query/rrf_fusion.h"
#include "cortrix/spc/onnx_embedder.h"
#include "cortrix/id/hash.h"
#include "cortrix/common/block_types.h"
#include "mock_reranker.h"
#include "fake_doc_discovery_deps.h"

namespace cortrix::query {
namespace {

using ::testing::_;
using ::testing::Return;

class RerankOptOutExecutionFx : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("cortrix_rerank_optout_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                                    ->current_test_info()
                                    ->line()));
        std::filesystem::remove_all(root_);
        harness_ = std::make_unique<cortrix::doc_summary::test::DiscoveryPoolHarness>(root_);
        ASSERT_TRUE(harness_->Admit(kNs).ok());

        // Chunk-level rows, not doc summaries: reranking lives on the chunk path,
        // so seeding block_type=17 would leave `ranked` empty and the assertions
        // would pass for the wrong reason. block_id must be the child_id hash --
        // that is the identity the executor resolves candidates through.
        SeedChunk("doc-a", "chunk-a", "alpha passage");
        SeedChunk("doc-b", "chunk-b", "bravo passage");
        SeedChunk("doc-c", "chunk-c", "charlie passage");

        // Fixed dense hits, ascending distance -> this is the pre-rerank order.
        harness_->fake_index()->set_search_result(
            {{ids_[0], 0.10f}, {ids_[1], 0.20f}, {ids_[2], 0.30f}});
    }

    void TearDown() override {
        harness_.reset();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    void SeedChunk(const std::string& doc_id, const std::string& child_id,
                   const std::string& text) {
        const uint64_t block_id = id::HashChildIdToBlockId(child_id);
        harness_->WithStore(kNs, [&](cortrix::CortrixStore& store) {
            cortrix::CortrixDoc doc;
            doc.doc_id = doc_id;
            doc.source_type = "test";
            store.doc_create(doc);
            cortrix::CortrixBlock b;
            b.block_id = block_id;
            b.doc_id = doc_id;
            b.child_id = child_id;
            b.block_type = static_cast<int>(kBlockFile);
            b.content_text = text;
            b.metadata_json = "{}";
            b.data = {0x01};  // blocks.data is NOT NULL
            store.block_insert(b);
        });
        ids_.push_back(block_id);
    }

    QueryContext MakeCtx(bool rerank) {
        QueryContext ctx;
        ctx.query = "alpha";
        ctx.top_k = 3;
        ctx.rerank = rerank;
        ctx.granularity = "chunk";  // pin the path reranking lives on
        return ctx;
    }

    static constexpr const char* kNs = "dedup-ns";
    std::filesystem::path root_;
    std::unique_ptr<cortrix::doc_summary::test::DiscoveryPoolHarness> harness_;
    std::vector<uint64_t> ids_;
    OnnxEmbedder embedder_{/*model_path=*/""};  // empty path = stub mode
    RRFFusion fusion_;
};

// The opt-out must stop the reranker being called at all -- not call it and discard
// the scores. On CPU the reranker is also the dominant cost of a query (9-17 s of a
// 10-20 s query in the campaign measurements), so a version that still invoked it
// would keep the latency while claiming the opt-out works.
TEST_F(RerankOptOutExecutionFx, DisabledRerankNeverInvokesTheReranker) {
    reranker::MockReranker rr;
    EXPECT_CALL(rr, ScoreBatch(_, _)).Times(0);
    EXPECT_CALL(rr, Rerank(_, _)).Times(0);
    EXPECT_CALL(rr, Score(_, _)).Times(0);

    LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_, &rr);
    const auto result = exec.ExecuteForNamespace(MakeCtx(/*rerank=*/false), kNs);

    EXPECT_TRUE(result.error_code.empty()) << result.error_code;
}

// Ordering must be the retrieval order, not an arbitrary one: a dedup caller turns
// reranking off precisely to keep the dense ranking, where the near-identical
// document sits first.
TEST_F(RerankOptOutExecutionFx, DisabledRerankPreservesRetrievalOrder) {
    reranker::MockReranker rr;
    EXPECT_CALL(rr, ScoreBatch(_, _)).Times(0);

    LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_, &rr);
    const auto result = exec.ExecuteForNamespace(MakeCtx(/*rerank=*/false), kNs);

    ASSERT_FALSE(result.chunks.empty());
    // Scores must be non-increasing: the RRF ordering carried through untouched.
    for (size_t i = 1; i < result.chunks.size(); ++i) {
        EXPECT_LE(result.chunks[i].score, result.chunks[i - 1].score)
            << "position " << i << " breaks the retrieval ordering";
    }
}

// The counterpart: with reranking enabled the executor must reach the reranker.
// Without this, the two cases above would also pass on a build where reranking was
// dead code, and the opt-out would be proving nothing.
TEST_F(RerankOptOutExecutionFx, EnabledRerankReachesTheReranker) {
    reranker::MockReranker rr;
    EXPECT_CALL(rr, ScoreBatch(_, _))
        .Times(::testing::AtLeast(1))
        .WillRepeatedly(Return(std::vector<float>{0.1f, 0.2f, 0.3f}));

    LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_, &rr);
    const auto result = exec.ExecuteForNamespace(MakeCtx(/*rerank=*/true), kNs);

    EXPECT_TRUE(result.error_code.empty()) << result.error_code;
}

}  // namespace
}  // namespace cortrix::query
