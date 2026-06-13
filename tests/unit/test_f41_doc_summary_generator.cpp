// F41 S1/S2 — DocSummaryGenerator: structured-output prompt, JSON parse (with
// max_chars truncation), short-doc single-call + long-doc map-reduce (§9.2),
// Generate end-to-end (success / no-chunks / LLM failure / null store), and the
// config resolver + doc_summary_status state machine.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/in_memory_global_config.h"
#include "cortrix/doc_summary/doc_summary_generator.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "mock_chunk_store.h"
#include "mock_llm_client.h"

namespace cortrix::doc_summary {
namespace {

using ::testing::_;
using ::testing::Return;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content = std::move(content);
    r.model = "gpt-4o-mini";
    r.prompt_tokens = 100;
    r.completion_tokens = 60;
    return r;  // status defaults ok()
}

const char* kGoodJson = R"({
  "summary_text": "This document covers Q3 2026 financials, revenue grew 23 percent.",
  "keywords": ["revenue", "finance", "Q3"],
  "topics": ["Financial Report"],
  "one_liner": "Q3 2026 financials"
})";

store::ChunkRecord Chunk(std::string id, std::string content) {
    store::ChunkRecord c;
    c.child_id = std::move(id);
    c.content = std::move(content);
    return c;
}

std::vector<store::ChunkRecord> NChunks(int n) {
    std::vector<store::ChunkRecord> out;
    for (int i = 0; i < n; ++i) {
        out.push_back(Chunk("c" + std::to_string(i), "chunk body " + std::to_string(i)));
    }
    return out;
}

DocSummaryGenerator MakeGen(std::shared_ptr<llm::MockLlmClient> llm,
                            std::shared_ptr<store::MockChunkStore> store,
                            DocSummaryConfig cfg = {}) {
    return DocSummaryGenerator(cfg, llm, store);
}

// ---------- ParseStructuredOutput ----------

TEST(F41DocSummaryGeneratorTest, ParseValidJson) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    auto r = g.ParseStructuredOutput(kGoodJson, 500);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_NE(r.value().summary_text.find("Q3 2026"), std::string::npos);
    EXPECT_EQ(r.value().keywords.size(), 3u);
    EXPECT_EQ(r.value().topics.size(), 1u);
    EXPECT_EQ(r.value().one_liner, "Q3 2026 financials");
}

TEST(F41DocSummaryGeneratorTest, ParseNonObjectIsInvalidOutput) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    auto r = g.ParseStructuredOutput("[1,2,3]", 500);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F41_LLM_INVALID_OUTPUT"),
              std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, ParseGarbageIsInvalidOutput) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    auto r = g.ParseStructuredOutput("not json at all", 500);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F41_LLM_INVALID_OUTPUT"),
              std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, ParseMissingSummaryTextIsInvalid) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    auto r = g.ParseStructuredOutput(R"({"keywords":["x"]})", 500);
    EXPECT_FALSE(r.ok());
}

TEST(F41DocSummaryGeneratorTest, ParseTruncatesSummaryToMaxChars) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string long_summary(1000, 'a');
    std::string j = R"({"summary_text":")" + long_summary + R"(","keywords":[],"topics":[],"one_liner":"x"})";
    auto r = g.ParseStructuredOutput(j, 500);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().summary_text.size(), 500u);
}

TEST(F41DocSummaryGeneratorTest, ParseTruncationIsUtf8Safe) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    // 10 CJK chars (3 bytes each in UTF-8) → truncating to 5 code points must not
    // split a multibyte sequence (result is valid UTF-8, <= 5 chars * 3 bytes).
    std::string cjk;
    for (int i = 0; i < 10; ++i) cjk += "\xe6\x96\x87";  // U+6587
    std::string j = R"({"summary_text":")" + cjk + R"(","keywords":[],"topics":[],"one_liner":""})";
    auto r = g.ParseStructuredOutput(j, 5);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().summary_text.size(), 15u);  // 5 chars * 3 bytes, clean cut
}

// ---------- Prompts ----------

TEST(F41DocSummaryGeneratorTest, BuildPromptHasStructuredRequirements) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string p = g.BuildPrompt("My Report", "the body text");
    EXPECT_NE(p.find("summary_text"), std::string::npos);
    EXPECT_NE(p.find("keywords"), std::string::npos);
    EXPECT_NE(p.find("topics"), std::string::npos);
    EXPECT_NE(p.find("one_liner"), std::string::npos);
    EXPECT_NE(p.find("My Report"), std::string::npos);
    EXPECT_NE(p.find("the body text"), std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, BuildGroupAndReducePrompts) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string grp = g.BuildGroupSummaryPrompt("Doc", "section text");
    EXPECT_NE(grp.find("section text"), std::string::npos);
    EXPECT_NE(grp.find("Doc"), std::string::npos);
    std::string red = g.BuildReducePrompt("Doc", {"partial A", "partial B"});
    EXPECT_NE(red.find("partial A"), std::string::npos);
    EXPECT_NE(red.find("partial B"), std::string::npos);
    EXPECT_NE(red.find("summary_text"), std::string::npos);  // structured output
}

// ---------- GenerateSummary: short path ----------

TEST(F41DocSummaryGeneratorTest, ShortDocSingleCall) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat(kGoodJson)));
    auto store = std::make_shared<store::MockChunkStore>();
    DocSummaryGenerator g = MakeGen(llm, store);  // chunk_threshold default 50

    bool mr = true;
    auto r = g.GenerateSummary(NChunks(5), "Report", &mr);
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(mr);  // short doc → no map-reduce
    EXPECT_NE(r.value().summary_text.find("Q3 2026"), std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, ShortDocLlmFailureIsTimeout) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("boom");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    DocSummaryGenerator g = MakeGen(llm, std::make_shared<store::MockChunkStore>());
    bool mr = false;
    auto r = g.GenerateSummary(NChunks(3), "R", &mr);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F41_LLM_TIMEOUT"), std::string::npos);
}

// ---------- GenerateSummary: map-reduce long path (§9.2) ----------

TEST(F41DocSummaryGeneratorTest, LongDocMapReduce) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    DocSummaryConfig cfg;
    cfg.chunk_threshold = 2;  // > 2 chunks → map-reduce
    // 5 chunks, group size 20 → 1 Map group (all 5) + 1 Reduce = 2 calls. Make the
    // first call a plain partial, the second the structured reduce output.
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(Return(OkChat("partial summary of the section")))
        .WillOnce(Return(OkChat(kGoodJson)));
    DocSummaryGenerator g = MakeGen(llm, std::make_shared<store::MockChunkStore>(), cfg);

    bool mr = false;
    auto r = g.GenerateSummary(NChunks(5), "Long Doc", &mr);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(mr);  // map-reduce path ran
    EXPECT_NE(r.value().summary_text.find("Q3 2026"), std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, MapReduceMapStageFailureSurfaces) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    DocSummaryConfig cfg;
    cfg.chunk_threshold = 2;
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("map boom");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));  // map stage fails
    DocSummaryGenerator g = MakeGen(llm, std::make_shared<store::MockChunkStore>(), cfg);
    bool mr = false;
    auto r = g.GenerateSummary(NChunks(5), "Long", &mr);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.status().message().find("CX_ERR_F41_LLM_TIMEOUT"), std::string::npos);
}

// ---------- Generate end-to-end ----------

TEST(F41DocSummaryGeneratorTest, GenerateSuccess) {
    DocSummaryMetrics::Instance().ResetForTest();
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat(kGoodJson)));
    auto store = std::make_shared<store::MockChunkStore>();
    EXPECT_CALL(*store, GetChunksByDocId("doc1")).WillOnce(Return(NChunks(4)));
    DocSummaryGenerator g = MakeGen(llm, store);

    GenerationResult res = g.Generate("doc1", "ns1");
    EXPECT_TRUE(res.success);
    EXPECT_FALSE(res.is_chunked);
    EXPECT_NE(res.summary.summary_text.find("Q3 2026"), std::string::npos);
    EXPECT_TRUE(res.embedding.empty());  // D3.5 pipeline wiring
    EXPECT_FALSE(res.error.has_value());
    EXPECT_EQ(DocSummaryMetrics::Instance().SummariesGeneratedCount(), 1u);
    DocSummaryMetrics::Instance().ResetForTest();
}

TEST(F41DocSummaryGeneratorTest, GenerateNoChunksIsDocTooLarge) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    auto store = std::make_shared<store::MockChunkStore>();
    EXPECT_CALL(*store, GetChunksByDocId("empty"))
        .WillOnce(Return(std::vector<store::ChunkRecord>{}));
    DocSummaryGenerator g = MakeGen(llm, store);

    GenerationResult res = g.Generate("empty", "ns1");
    EXPECT_FALSE(res.success);
    ASSERT_TRUE(res.error.has_value());
    EXPECT_EQ(res.error->code, "CX_ERR_F41_DOC_TOO_LARGE");
}

TEST(F41DocSummaryGeneratorTest, GenerateLlmFailureCarriesTimeoutError) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse fail;
    fail.status = Status::Unavailable("net");
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(fail));
    auto store = std::make_shared<store::MockChunkStore>();
    EXPECT_CALL(*store, GetChunksByDocId("doc1")).WillOnce(Return(NChunks(3)));
    DocSummaryGenerator g = MakeGen(llm, store);

    GenerationResult res = g.Generate("doc1", "ns1");
    EXPECT_FALSE(res.success);
    ASSERT_TRUE(res.error.has_value());
    EXPECT_EQ(res.error->code, "CX_ERR_F41_LLM_TIMEOUT");
}

TEST(F41DocSummaryGeneratorTest, GenerateInvalidOutputCarriesInvalidError) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(OkChat("not json")));
    auto store = std::make_shared<store::MockChunkStore>();
    EXPECT_CALL(*store, GetChunksByDocId("doc1")).WillOnce(Return(NChunks(2)));
    DocSummaryGenerator g = MakeGen(llm, store);

    GenerationResult res = g.Generate("doc1", "ns1");
    EXPECT_FALSE(res.success);
    ASSERT_TRUE(res.error.has_value());
    EXPECT_EQ(res.error->code, "CX_ERR_F41_LLM_INVALID_OUTPUT");
}

TEST(F41DocSummaryGeneratorTest, GenerateNullChunkStoreErrors) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    DocSummaryGenerator g(DocSummaryConfig{}, llm, /*chunk_store=*/nullptr);
    GenerationResult res = g.Generate("doc1", "ns1");
    EXPECT_FALSE(res.success);
    ASSERT_TRUE(res.error.has_value());
}

// ---------- Config resolver (§4.4) ----------

TEST(F41DocSummaryConfigTest, ResolveNullGlobalGivesDefaults) {
    DocSummaryConfig cfg = ResolveDocSummaryConfig(nullptr);
    EXPECT_EQ(cfg.max_chars, kMaxCharsDefault);
    EXPECT_EQ(cfg.chunk_threshold, kChunkThresholdDefault);
    EXPECT_EQ(cfg.prompt_version, kPromptVersionDefault);
    EXPECT_EQ(cfg.fts5_fallback_enabled, kFts5FallbackEnabledDefault);
}

TEST(F41DocSummaryConfigTest, ResolveReadsGlobalLayer) {
    InMemoryGlobalConfig gc;
    gc.Set(kMaxCharsKey, "300");
    gc.Set(kChunkThresholdKey, "10");
    gc.Set(kPromptVersionKey, "v2");
    gc.Set(kFts5FallbackEnabledKey, "false");
    DocSummaryConfig cfg = ResolveDocSummaryConfig(&gc);
    EXPECT_EQ(cfg.max_chars, 300);
    EXPECT_EQ(cfg.chunk_threshold, 10);
    EXPECT_EQ(cfg.prompt_version, "v2");
    EXPECT_FALSE(cfg.fts5_fallback_enabled);
}

TEST(F41DocSummaryConfigTest, ResolveIgnoresNonPositiveAndUnparseable) {
    InMemoryGlobalConfig gc;
    gc.Set(kMaxCharsKey, "0");          // non-positive → keep default
    gc.Set(kChunkThresholdKey, "abc");  // unparseable → keep default
    DocSummaryConfig cfg = ResolveDocSummaryConfig(&gc);
    EXPECT_EQ(cfg.max_chars, kMaxCharsDefault);
    EXPECT_EQ(cfg.chunk_threshold, kChunkThresholdDefault);
}

// ---------- doc_summary_status state machine (§7.1) ----------

TEST(F41DocSummaryStatusTest, ToStringValues) {
    EXPECT_STREQ(ToString(DocSummaryStatus::kPending), "pending");
    EXPECT_STREQ(ToString(DocSummaryStatus::kGenerated), "generated");
    EXPECT_STREQ(ToString(DocSummaryStatus::kFailed), "failed");
}

}  // namespace
}  // namespace cortrix::doc_summary
