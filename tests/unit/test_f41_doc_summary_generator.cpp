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
#include "cortrix/doc_summary/doc_summary_error.h"
#include "cortrix/doc_summary/doc_summary_metrics.h"
#include "mock_chunk_store.h"
#include "mock_llm_client.h"

namespace cortrix::doc_summary {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::Field;
using ::testing::Return;
using ::testing::SaveArg;

llm::ChatCompletionResponse OkChat(std::string content) {
    llm::ChatCompletionResponse r;
    r.content_length = static_cast<int>(content.size());
    r.content = std::move(content);
    r.content_source = "message.content";
    r.model = "gpt-4o-mini";
    r.finish_reason = "stop";
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

TEST(F41DocSummaryGeneratorTest, ParseCompleteFencedJson) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    auto r = g.ParseStructuredOutput(std::string("```json\n") + kGoodJson + "\n```", 500);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_NE(r.value().summary_text.find("Q3 2026"), std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, ParseRepairsInvalidBackslashEscapeInJsonString) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    const char* deepseek_style_json = R"({
      "summary_text": "This document includes an escaped shrug example: ¯\_(ツ)_/¯.",
      "keywords": ["summary", "escape", "json"],
      "topics": ["Technical Documentation"],
      "one_liner": "Invalid backslash escape"
    })";

    auto r = g.ParseStructuredOutput(deepseek_style_json, 500);

    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_NE(r.value().summary_text.find(R"(¯\_(ツ)_/¯)"), std::string::npos);
    EXPECT_EQ(r.value().keywords.size(), 3u);
}

TEST(F41DocSummaryGeneratorTest, ParseMissingSummaryTextIsInvalid) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    auto r = g.ParseStructuredOutput(R"({"keywords":["x"]})", 500);
    EXPECT_FALSE(r.ok());
}

// ---------- Parse-repair second chance (deep-QA 2026-07-10, DeepSeek field
// failure: valid object wrapped in prose or an UNCLOSED fence rejected by the
// strict path -> CX_ERR_F41_LLM_INVALID_OUTPUT mid-drain) ----------

TEST(F41DocSummaryGeneratorTest, ParseRepairsProseWrappedJson) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string repair;
    auto r = g.ParseStructuredOutput(
        std::string("Here is the JSON you asked for:\n") + kGoodJson +
            "\nLet me know if you need anything else.",
        500, &repair);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_NE(r.value().summary_text.find("Q3 2026"), std::string::npos);
    EXPECT_EQ(repair, "balanced_extract");
}

TEST(F41DocSummaryGeneratorTest, ParseRepairsProseWrappedJsonWithInvalidEscape) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    const char* wrapped = R"(Here is the requested object:
{
  "summary_text": "A shrug example: ¯\_(ツ)_/¯.",
  "keywords": ["summary", "escape"],
  "topics": ["Technical Documentation"],
  "one_liner": "Wrapped invalid escape"
}
End of response.)";
    std::string repair;

    auto r = g.ParseStructuredOutput(wrapped, 500, &repair);

    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_NE(r.value().summary_text.find(R"(¯\_(ツ)_/¯)"), std::string::npos);
    EXPECT_EQ(repair, "balanced_extract_invalid_string_escape");
}

TEST(F41DocSummaryGeneratorTest, ParseRepairsUnclosedFence) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string repair;
    auto r = g.ParseStructuredOutput(std::string("```json\n") + kGoodJson, 500,
                                     &repair);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(repair, "balanced_extract");
}

TEST(F41DocSummaryGeneratorTest, ParseRepairHonorsBracesInsideStrings) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string repair;
    auto r = g.ParseStructuredOutput(
        "prefix {\"summary_text\": \"has a brace } and quote \\\" inside\", "
        "\"keywords\": [\"k\"], \"topics\": [\"t\"], \"one_liner\": \"o\"} suffix",
        500, &repair);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_NE(r.value().summary_text.find("has a brace }"), std::string::npos);
    EXPECT_EQ(repair, "balanced_extract");
}

TEST(F41DocSummaryGeneratorTest, ParseTruncatedJsonStillFails) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string repair;
    auto r = g.ParseStructuredOutput(
        R"({"summary_text": "cut off mid-genera)", 500, &repair);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(repair.empty());
    EXPECT_NE(r.status().message().find("CX_ERR_F41_LLM_INVALID_OUTPUT"),
              std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, ParseStrictPathSetsNoRepairMarker) {
    DocSummaryGenerator g = MakeGen(std::make_shared<llm::MockLlmClient>(),
                                    std::make_shared<store::MockChunkStore>());
    std::string repair = "stale";
    auto r = g.ParseStructuredOutput(kGoodJson, 500, &repair);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(repair.empty());
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

TEST(F41DocSummaryGeneratorTest, StructuredCallUsesDeepSeekJsonContract) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(DoAll(SaveArg<1>(&captured), Return(OkChat(kGoodJson))));
    auto store = std::make_shared<store::MockChunkStore>();
    DocSummaryConfig cfg;
    cfg.llm_model = "deepseek-v4-flash";
    DocSummaryGenerator g = MakeGen(llm, store, cfg);

    bool mr = true;
    auto r = g.GenerateSummary(NChunks(5), "Report", &mr);

    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(mr);
    EXPECT_EQ(captured.model, "deepseek-v4-flash");
    EXPECT_EQ(captured.max_tokens, 4096);
    EXPECT_EQ(captured.response_format, "json_object");
    EXPECT_EQ(captured.thinking_type, "disabled");
    EXPECT_FALSE(captured.allow_reasoning_content_fallback);
}

TEST(F41DocSummaryGeneratorTest, StructuredCallKeepsGlmPromptOnlyContract) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::LlmCallConfig captured;
    EXPECT_CALL(*llm, Chat(_, _))
        .WillOnce(DoAll(SaveArg<1>(&captured), Return(OkChat(kGoodJson))));
    auto store = std::make_shared<store::MockChunkStore>();
    DocSummaryConfig cfg;
    cfg.llm_model = "glm-5.2";
    DocSummaryGenerator g = MakeGen(llm, store, cfg);

    bool mr = true;
    auto r = g.GenerateSummary(NChunks(5), "Report", &mr);

    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_FALSE(mr);
    EXPECT_EQ(captured.model, "glm-5.2");
    EXPECT_EQ(captured.max_tokens, 4096);
    EXPECT_EQ(captured.response_format, "");
    EXPECT_EQ(captured.thinking_type, "");
    EXPECT_FALSE(captured.allow_reasoning_content_fallback);
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

// Regression (DEFECT#3, 2026-06-26): the generator must send the CONFIGURED
// llm_model, not the kDefaultLlmModel "gpt-4o-mini". A live GLM deployment
// (doc_summary_llm.model=glm-5.2) was sending "gpt-4o-mini" — the DocSummaryConfig
// default, never overridden by the bootstrap wiring — which GLM rejects with HTTP
// 400, so every summary silently failed. Guard that config_.llm_model reaches the
// LLM call. (The bootstrap-side wiring is verified by the live E2E; this unit guard
// catches any regression that re-hardcodes the model in the generator.)
TEST(F41DocSummaryGeneratorTest, SendsConfiguredModelNotDefault) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    DocSummaryConfig cfg;
    cfg.llm_model = "glm-5.2";
    EXPECT_CALL(*llm, Chat(_, Field(&llm::LlmCallConfig::model, "glm-5.2")))
        .WillOnce(Return(OkChat(kGoodJson)));
    DocSummaryGenerator g = MakeGen(llm, std::make_shared<store::MockChunkStore>(), cfg);
    bool mr = false;
    auto r = g.GenerateSummary(NChunks(3), "Report", &mr);
    ASSERT_TRUE(r.ok()) << r.status().message();
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

TEST(F41DocSummaryGeneratorTest, GenerateNoChunksCompletesWithoutSummaryContent) {
    DocSummaryMetrics::Instance().ResetForTest();
    auto llm = std::make_shared<llm::MockLlmClient>();
    auto store = std::make_shared<store::MockChunkStore>();
    EXPECT_CALL(*store, GetChunksByDocId("empty"))
        .WillOnce(Return(std::vector<store::ChunkRecord>{}));
    EXPECT_CALL(*llm, Chat(_, _)).Times(0);
    DocSummaryGenerator g = MakeGen(llm, store);

    GenerationResult res = g.Generate("empty", "ns1");
    EXPECT_TRUE(res.success);
    EXPECT_TRUE(res.no_summary_content);
    EXPECT_FALSE(res.error.has_value());
    EXPECT_TRUE(res.summary.summary_text.empty());
    EXPECT_EQ(DocSummaryMetrics::Instance().SummariesGeneratedCount(), 0u);
    DocSummaryMetrics::Instance().ResetForTest();
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
    ASSERT_TRUE(res.error->structured_data.has_value());
    EXPECT_TRUE(HasRequiredStructuredData(DocSummaryErrorCode::kLlmTimeout,
                                          *res.error->structured_data));
    EXPECT_EQ((*res.error->structured_data)["doc_id"], "doc1");
    EXPECT_EQ((*res.error->structured_data)["attempt_count"], 1);
    EXPECT_NE((*res.error->structured_data)["last_error_message"].get<std::string>().find("net"),
              std::string::npos);
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
    ASSERT_TRUE(res.error->structured_data.has_value());
    EXPECT_TRUE(HasRequiredStructuredData(DocSummaryErrorCode::kLlmInvalidOutput,
                                          *res.error->structured_data));
    EXPECT_EQ((*res.error->structured_data)["doc_id"], "doc1");
    EXPECT_EQ((*res.error->structured_data)["raw_output_preview"], "not json");
    EXPECT_EQ((*res.error->structured_data)["llm_content_source"], "message.content");
    EXPECT_EQ((*res.error->structured_data)["llm_model"], "gpt-4o-mini");
    EXPECT_EQ((*res.error->structured_data)["llm_finish_reason"], "stop");
    EXPECT_EQ((*res.error->structured_data)["llm_content_length"], 8);
    EXPECT_EQ((*res.error->structured_data)["llm_reasoning_content_length"], 0);
    EXPECT_NE((*res.error->structured_data)["parse_error"].get<std::string>().find(
                  "CX_ERR_F41_LLM_INVALID_OUTPUT"),
              std::string::npos);
    EXPECT_NE(res.error->message.find("content_source=message.content"),
              std::string::npos);
}

TEST(F41DocSummaryGeneratorTest, GenerateInvalidOutputCapturesReasoningDiagnostics) {
    auto llm = std::make_shared<llm::MockLlmClient>();
    llm::ChatCompletionResponse response = OkChat("not json");
    response.model = "glm-5.2";
    response.finish_reason = "stop";
    response.reasoning_content_length = 1234;
    EXPECT_CALL(*llm, Chat(_, _)).WillOnce(Return(response));
    auto store = std::make_shared<store::MockChunkStore>();
    EXPECT_CALL(*store, GetChunksByDocId("doc1")).WillOnce(Return(NChunks(2)));
    DocSummaryGenerator g = MakeGen(llm, store);

    GenerationResult res = g.Generate("doc1", "ns1");
    EXPECT_FALSE(res.success);
    ASSERT_TRUE(res.error.has_value());
    ASSERT_TRUE(res.error->structured_data.has_value());
    EXPECT_EQ((*res.error->structured_data)["llm_model"], "glm-5.2");
    EXPECT_EQ((*res.error->structured_data)["llm_reasoning_content_length"], 1234);
    EXPECT_NE(res.error->message.find("reasoning_content_length=1234"),
              std::string::npos);
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
