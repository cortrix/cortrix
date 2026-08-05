#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/spc/hype_block.h"
#include "cortrix/spc/hype_enricher.h"
#include "cortrix/spc/hype_fusion.h"
#include "cortrix/spc/hype_metrics.h"
#include "mock_embedder.h"
#include "mock_llm_client.h"
#include "mock_parent_chunk_store.h"

// HyPE S7 — standalone integration: the full HyPE pipeline wired together
// against the mock LLM + mock ParentChunkStore + stub embedder. Covers the
// generate -> embed -> Block(type=16) -> fuse -> explain chain + the L2
// degrade (IT-F38-recall-4). Real BEIR Recall@10 +3pp / hit-rate 30% / LLM
// failure rate <5% (§13.bis) = D3.5 (real model + dataset not present).
namespace cortrix::spc {
namespace {

using ::testing::_;
using ::testing::Return;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    return r;
}

DocumentMetadata DocMeta() {
    DocumentMetadata m;
    m.doc_title = "FY2026 Report";
    return m;
}

// Case 1 — generate -> embed -> hype_question Block (block_type=16) end-to-end.
TEST(F38HypeIntegrationTest, IT1_GenerateEmbedBuildBlock) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(Return(OkChat("What was Q1 revenue?\n"
                                "What was the growth rate?\n"
                                "Why did revenue grow?")));
    HyPEEnricher enr(HyPEConfig{}, llm, nullptr);

    auto qs = enr.GenerateHypeQuestions("Q1 revenue 520M +23%", /*parent=*/"",
                                        DocMeta(), "child_1", "parent_1");
    ASSERT_TRUE(qs.ok());
    ASSERT_EQ(qs.value().size(), 3u);

    cortrix::testing::FakeEmbedder emb(1024);
    for (auto& q : qs.value()) {
        ASSERT_TRUE(FillHypeEmbedding(q, emb).ok());
        auto blob = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0001,
                                           "2026-05-19T12:00:00Z");
        const cortrix_block_header_t* hdr = nullptr;
        ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
        EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(kBlockHypeQuestion));
        EXPECT_EQ(hdr->vector_dim, 1024);
    }
}

// IT — parent_text reverse-lookup threads into the prompt (reconcile 2 happy path).
TEST(F38HypeIntegrationTest, IT_ParentTextResolvedAndUsed) {
    auto store = std::make_shared<store::MockParentChunkStore>();
    cortrix::chunker::ParentChunk parent;
    parent.parent_id = "p1";
    parent.parent_text = "FULL PARENT SECTION TEXT";
    EXPECT_CALL(*store, GetParent("p1"))
        .WillOnce(Return(Result<cortrix::chunker::ParentChunk>(parent)));

    auto llm = std::make_shared<llm::MockLlmClient>();
    std::string captured;
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce([&](const std::string& prompt, const llm::LlmCallConfig&) {
            captured = prompt;
            return OkChat("q1\nq2\nq3");
        });
    HyPEEnricher enr(HyPEConfig{}, llm, store);

    auto pt = enr.ResolveParentText("p1");
    ASSERT_TRUE(pt.ok());
    auto qs = enr.GenerateHypeQuestions("chunk", pt.value(), DocMeta(), "c", "p1");
    ASSERT_TRUE(qs.ok());
    EXPECT_NE(captured.find("FULL PARENT SECTION TEXT"), std::string::npos);
}

// Case 5 — mixed-pool fusion: a chunk recalled via hype gets via_hype + explain.
TEST(F38HypeIntegrationTest, IT5_FusionExplainViaHype) {
    std::vector<HypeCandidate> pool;
    // chunk path: c1 (rank0), c2 (rank1)
    pool.push_back({"c1", 0, 0.8f, "", ""});
    pool.push_back({"c2", 0, 0.4f, "", ""});
    // hype path: a question pointing at c2 (lifts c2 + sets explain)
    HypeCandidate h;
    h.block_type = cortrix::kBlockHypeQuestion;
    h.score = 0.95f;
    h.source_child_id = "c2";
    h.question_text = "How much did c2 earn?";
    pool.push_back(h);

    std::map<ChildId, ParentId> parent_of = {{"c1", "p1"}, {"c2", "p2"}};
    auto out = FuseHypeRecall(pool, parent_of, /*top_n=*/10);
    ASSERT_EQ(out.size(), 2u);  // distinct parents

    const HypeFusedResult* c2 = nullptr;
    for (const auto& r : out) if (r.child_id == "c2") c2 = &r;
    ASSERT_TRUE(c2);
    EXPECT_TRUE(c2->via_hype);
    EXPECT_EQ(c2->hype_question_matched, "How much did c2 earn?");
    EXPECT_FLOAT_EQ(c2->hype_match_score, 0.95f);
}

// IT-F38-recall-4 — L2 degrade: LLM mock failure → Enrich reports failed, the
// chunk is NOT blocked (HyPE skipped). Standalone proxy for "Recall@10 not
// below dense-only -1pp" (the real recall measurement = D3.5).
TEST(F38HypeIntegrationTest, IT_recall4_LlmFailureDegrades) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("LLM down");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    HyPEEnricher enr(HyPEConfig{}, llm, nullptr);

    EnrichResult res = enr.Enrich("chunk", DocMeta(), ChunkContext{});
    EXPECT_FALSE(res.ok());            // HyPE failed
    EXPECT_FALSE(res.error_msg.empty());
    // The enricher does not throw / crash — upstream still writes the chunk Block
    // (degrade to chunk-only embedding). Fusion of a chunk-only pool still works:
    std::vector<HypeCandidate> chunk_only = {{"c1", 0, 0.9f, "", ""}};
    auto out = FuseHypeRecall(chunk_only, {});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_FALSE(out[0].via_hype);     // no hype path → dense/chunk recall intact
}

// IT — metrics flow along the pipeline (§11 recorder).
TEST(F38HypeIntegrationTest, IT_MetricsRecorded) {
    HypeMetrics::Instance().ResetForTest();
    using LS = HypeMetrics::LlmCallStatus;
    using HT = HypeMetrics::MatchHitType;

    HypeMetrics::Instance().RecordLlmCall(LS::kSuccess);
    HypeMetrics::Instance().AddQuestionsGenerated(3);
    HypeMetrics::Instance().ObserveLlmDuration("gpt-4o-mini", 0.4);
    HypeMetrics::Instance().RecordMatch(HT::kBoth);

    EXPECT_EQ(HypeMetrics::Instance().LlmCallCount(LS::kSuccess), 1u);
    EXPECT_EQ(HypeMetrics::Instance().QuestionsGeneratedCount(), 3u);
    EXPECT_EQ(HypeMetrics::Instance().MatchCount(HT::kBoth), 1u);
    EXPECT_EQ(HypeMetrics::Instance().LlmDurationCount("gpt-4o-mini"), 1u);
    HypeMetrics::Instance().ResetForTest();
}

}  // namespace
}  // namespace cortrix::spc
