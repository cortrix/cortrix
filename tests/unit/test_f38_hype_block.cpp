#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "cortrix/common/block_header.h"
#include "cortrix/common/block_types.h"
#include "cortrix/spc/hype_block.h"
#include "mock_embedder.h"

// HyPE S3 — hype_question Block generation (block_type=16 + metadata_json
// source_child_id/source_parent_id + hype{} + embedding via OnnxEmbedder mock).
// The atomic write coordinator PWL write (chunk + hype_question same txn) = D3.5.
namespace cortrix::spc {
namespace {

HypeQuestion MakeQ(std::string text, int idx, std::string child, std::string parent) {
    HypeQuestion q;
    q.question_text = std::move(text);
    q.question_index = idx;
    q.source_child_id = std::move(child);
    q.source_parent_id = std::move(parent);
    return q;
}

// ---------- metadata_json (§4.2) ----------

TEST(F38HypeBlockTest, MetadataJsonShape) {
    HypeQuestion q = MakeQ("What was Q1 revenue?", 2, "child_7", "parent_3");
    std::string js = BuildHypeQuestionMetadataJson(q, "v1", "gpt-4o-mini", 0.0003,
                                                   "2026-05-19T12:00:00Z");
    auto j = nlohmann::json::parse(js);
    EXPECT_EQ(j["source_child_id"], "child_7");
    EXPECT_EQ(j["source_parent_id"], "parent_3");
    ASSERT_TRUE(j.contains("hype"));
    EXPECT_EQ(j["hype"]["question_index"], 2);
    EXPECT_EQ(j["hype"]["prompt_version"], "v1");
    EXPECT_EQ(j["hype"]["llm_provider"], "openai");
    EXPECT_EQ(j["hype"]["llm_model"], "gpt-4o-mini");
    EXPECT_DOUBLE_EQ(j["hype"]["cost_usd"].get<double>(), 0.0003);
    EXPECT_EQ(j["hype"]["generated_at"], "2026-05-19T12:00:00Z");
}

TEST(F38HypeBlockTest, MetadataJsonCustomProvider) {
    HypeQuestion q = MakeQ("q", 0, "c", "p");
    std::string js = BuildHypeQuestionMetadataJson(q, "v1", "claude-haiku", 0.0,
                                                   "t", "anthropic");
    auto j = nlohmann::json::parse(js);
    EXPECT_EQ(j["hype"]["llm_provider"], "anthropic");
}

// ---------- Block BLOB build + round-trip ----------

TEST(F38HypeBlockTest, BuildsBlockWithType16) {
    HypeQuestion q = MakeQ("How much revenue?", 0, "c1", "p1");
    q.embedding.assign(1024, 0.0f);
    auto blob = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0001,
                                       "2026-05-19T12:00:00Z");
    ASSERT_FALSE(blob.empty());

    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));  // magic+version+CRC ok
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->block_type, static_cast<uint16_t>(kBlockHypeQuestion));  // 16
    EXPECT_EQ(hdr->vector_dim, 1024);
    EXPECT_EQ(hdr->flags_ext, 0);  // HyPE owns no flags_ext bit (type=16 discriminates)
}

TEST(F38HypeBlockTest, BlockContentIsQuestionText) {
    HypeQuestion q = MakeQ("What drove the growth?", 1, "c", "p");
    auto blob = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0, "t");
    // The question text appears verbatim in the payload (content section).
    std::string s(blob.begin(), blob.end());
    EXPECT_NE(s.find("What drove the growth?"), std::string::npos);
    EXPECT_NE(s.find("source_child_id"), std::string::npos);
}

TEST(F38HypeBlockTest, BlockEnrichmentSourceIsLlm) {
    HypeQuestion q = MakeQ("q", 0, "c", "p");
    auto blob = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0, "t");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->enrichment_source, static_cast<uint8_t>(kEnrichLlm));
}

TEST(F38HypeBlockTest, BlockCrcDetectsTamper) {
    HypeQuestion q = MakeQ("q", 0, "c", "p");
    auto blob = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0, "t");
    // Corrupt a payload byte → CRC parse must fail (well-formed Block).
    blob[blob.size() - 1] ^= 0xFF;
    const cortrix_block_header_t* hdr = nullptr;
    EXPECT_FALSE(BlockParse(blob.data(), blob.size(), &hdr));
}

// ---------- embedding fill (§9.1) ----------

TEST(F38HypeBlockTest, FillEmbeddingFromEmbedder) {
    cortrix::testing::FakeEmbedder emb(1024);
    HypeQuestion q = MakeQ("embed me", 0, "c", "p");
    ASSERT_TRUE(q.embedding.empty());
    Status s = FillHypeEmbedding(q, emb);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(q.embedding.size(), 1024u);
}

TEST(F38HypeBlockTest, FilledEmbeddingFlowsIntoBlockVectorDim) {
    cortrix::testing::FakeEmbedder emb(384);
    HypeQuestion q = MakeQ("dim test", 0, "c", "p");
    ASSERT_TRUE(FillHypeEmbedding(q, emb).ok());
    auto blob = BuildHypeQuestionBlock(q, "v1", "gpt-4o-mini", 0.0, "t");
    const cortrix_block_header_t* hdr = nullptr;
    ASSERT_TRUE(BlockParse(blob.data(), blob.size(), &hdr));
    EXPECT_EQ(hdr->vector_dim, 384);  // embedding dim propagated
}

}  // namespace
}  // namespace cortrix::spc
