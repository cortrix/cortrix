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
        // Term frequency differs so the retrieval ranking is a strict order rather
        // than a three-way tie. Equal-scoring candidates order by internal block id,
        // which shifts with process-global state (the deployment hash key is set by
        // whichever test runs first), so a tie makes the expected sequence unstable
        // in a full-suite run while passing in isolation.
        SeedChunk("doc-a", "chunk-a", "match match match match alpha");
        SeedChunk("doc-b", "chunk-b", "match match bravo");
        SeedChunk("doc-c", "chunk-c", "match charlie");

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

    static std::vector<std::string> ChildIds(
            const retrieval::NamespaceQueryResult& r) {
        std::vector<std::string> out;
        out.reserve(r.chunks.size());
        for (const auto& c : r.chunks) out.push_back(std::string(c.child_id));
        return out;
    }

    QueryContext MakeCtx(bool rerank) {
        QueryContext ctx;
        ctx.query = "match";
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

// rerank=false must return the retrieval ranking itself, by identity and in order.
//
// Asserting only that scores descend would pass on an executor that permuted the
// documents and then handed out descending scores -- which is precisely the failure
// a dedup caller cares about, since the document that moves is their answer.
//
// The expected sequence is the retrieval ranking for this query over the seeded
// texts: chunk-a matches "match" four times, chunk-b twice, chunk-c once. The
// enabled case below returns the reverse order from the same inputs, which is what
// gives this assertion its meaning.
TEST_F(RerankOptOutExecutionFx, DisabledRerankReturnsTheRetrievalRankingByIdentity) {
    reranker::MockReranker rr;
    EXPECT_CALL(rr, ScoreBatch(_, _)).Times(0);

    LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_, &rr);
    const auto result = exec.ExecuteForNamespace(MakeCtx(/*rerank=*/false), kNs);

    EXPECT_EQ(ChildIds(result), (std::vector<std::string>{"chunk-a", "chunk-b", "chunk-c"}))
        << "rerank=false must pass the retrieval ranking through untouched";
}

// The counterpart, and the reason the case above means anything: with reranking on,
// the same inputs come back in a different identity order -- the one the reranker
// asked for. Without this, both disabled cases would also pass on a build where
// reranking never ran at all.
TEST_F(RerankOptOutExecutionFx, EnabledRerankReordersByRerankerScore) {
    reranker::MockReranker rr;
    // Score by content so the reranker's preference is the exact inverse of the
    // retrieval ranking: charlie > bravo > alpha.
    EXPECT_CALL(rr, ScoreBatch(_, _))
        .Times(::testing::AtLeast(1))
        .WillRepeatedly([](const char*, const std::vector<const char*>& passages) {
            std::vector<float> out;
            out.reserve(passages.size());
            for (const char* p : passages) {
                const std::string text = p == nullptr ? "" : p;
                if (text.find("charlie") != std::string::npos)      out.push_back(0.90f);
                else if (text.find("bravo") != std::string::npos)   out.push_back(0.50f);
                else                                                out.push_back(0.10f);
            }
            return out;
        });

    LiveSingleUnitExecutor exec(harness_->ipool(), embedder_, fusion_, &rr);
    const auto result = exec.ExecuteForNamespace(MakeCtx(/*rerank=*/true), kNs);

    EXPECT_EQ(ChildIds(result), (std::vector<std::string>{"chunk-c", "chunk-b", "chunk-a"}))
        << "rerank=true must return the reranker's ordering, not the retrieval one";
}

}  // namespace
}  // namespace cortrix::query
