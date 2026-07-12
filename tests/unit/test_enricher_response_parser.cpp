#include "cortrix/spc_enricher/enricher_response_parser.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "cortrix/spc_enricher.h"
#include "cortrix/spc_enricher/enricher_error.h"

namespace cortrix::spc {
namespace {

// A well-formed 2-chunk response.
const char* kGoodBody = R"({
  "0": {"entities":[{"text":"Cortrix","type":"PRODUCT","start_offset":0,"end_offset":7}],
        "summary":"about Cortrix","score":0.9},
  "1": {"entities":[],"summary":"second chunk","score":0.5}
})";

TEST(EnricherResponseParserTest, HappyPathFillsFields) {
    auto r = ParseEnrichBatchResponse(kGoodBody, 2, "gpt-4o-mini", "default-zh");
    ASSERT_EQ(r.size(), 2u);

    EXPECT_TRUE(r[0].ok());
    ASSERT_EQ(r[0].entities.size(), 1u);
    EXPECT_EQ(r[0].entities[0].text, "Cortrix");
    EXPECT_EQ(r[0].entities[0].type, "PRODUCT");
    EXPECT_EQ(r[0].entities[0].end_offset, 7);
    EXPECT_EQ(r[0].summary, "about Cortrix");
    EXPECT_FLOAT_EQ(r[0].enriched_score, 0.9f);
    EXPECT_EQ(r[0].model_used, "gpt-4o-mini");
    EXPECT_EQ(r[0].prompt_version, "default-zh");
    EXPECT_EQ(r[0].enricher_name, "LlmEnricher");

    EXPECT_TRUE(r[1].ok());
    EXPECT_TRUE(r[1].entities.empty());
    EXPECT_EQ(r[1].summary, "second chunk");
}

TEST(EnricherResponseParserTest, ResultSizeAlwaysBatchSize) {
    // More indices requested than present → padded with L2 errors.
    auto r = ParseEnrichBatchResponse(R"({"0":{"summary":"a","score":0.1}})", 3,
                                      "m", "default-zh");
    ASSERT_EQ(r.size(), 3u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_FALSE(r[1].ok());
    EXPECT_FALSE(r[2].ok());
}

TEST(EnricherResponseParserTest, L3WholeBatchParseFail) {
    auto r = ParseEnrichBatchResponse("this is not json", 2, "m", "default-zh");
    ASSERT_EQ(r.size(), 2u);
    for (const auto& res : r) {
        EXPECT_EQ(res.status, static_cast<int>(EnricherErrorCode::kParse));
        EXPECT_FLOAT_EQ(res.enriched_score, 0.0f);
        auto sd = nlohmann::json::parse(res.error_meta.structured_data);
        EXPECT_EQ(sd["layer"], "L3");
        EXPECT_EQ(sd["batch_size"], 2);
        // PARSE is permanent / not retryable (§5.1).
        EXPECT_FALSE(res.error_meta.retryable);
        EXPECT_EQ(res.error_meta.category, agent_friendly::ErrorCategory::kPermanent);
    }
}

TEST(EnricherResponseParserTest, L3RepairsInvalidBackslashEscape) {
    // A provider echoing a Windows path emits the invalid \U escape; strict
    // parsing refuses the WHOLE batch (L3 all-or-nothing), so pre-repair one
    // bad chunk poisoned every neighbor and retries failed deterministically
    // on the same text (QA 2026-07-12 F-10). The shared escape repair must
    // rescue the batch and preserve the author-visible text.
    // The neighbor carries a LEGAL escape (\n): the repair must leave valid
    // escapes untouched — an over-escaping regression would corrupt it.
    const char* body =
        "{\"0\":{\"summary\":\"path C:\\Users broke it\",\"score\":0.5},"
        "\"1\":{\"summary\":\"innocent\\nneighbor\",\"score\":0.4}}";
    auto r = ParseEnrichBatchResponse(body, 2, "m", "default-zh");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].summary, "path C:\\Users broke it");
    EXPECT_TRUE(r[1].ok());
    EXPECT_EQ(r[1].summary, "innocent\nneighbor");
}

TEST(EnricherResponseParserTest, L3RepairsInvalidEscapeInsideJsonFence) {
    // Fence unwrap runs BEFORE the escape repair; the combination (fenced body
    // whose payload carries a bad escape) must still be rescued.
    const char* body =
        "```json\n"
        "{\"0\":{\"summary\":\"path C:\\Users again\",\"score\":0.5}}\n"
        "```";
    auto r = ParseEnrichBatchResponse(body, 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].summary, "path C:\\Users again");
}

TEST(EnricherResponseParserTest, L3RepairsConsecutiveInvalidEscapes) {
    // Two invalid escapes back to back: the repair must reset after each
    // emitted escape and double BOTH independently.
    const char* body =
        "{\"0\":{\"summary\":\"dirs \\D\\W here\",\"score\":0.5}}";
    auto r = ParseEnrichBatchResponse(body, 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].summary, "dirs \\D\\W here");
}

TEST(EnricherResponseParserTest, L3RepairsInvalidEscapeInObjectKey) {
    // The bad escape sits in a sibling KEY, not a value — the repair walks
    // every JSON string including keys, so one poisoned key must not sink the
    // batch (L3 is all-or-nothing).
    const char* body =
        "{\"0\":{\"summary\":\"ok\",\"score\":0.5},\"we\\Uird\":1}";
    auto r = ParseEnrichBatchResponse(body, 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].summary, "ok");
}

TEST(EnricherResponseParserTest, L3RepairPreservesValidUnicodeEscape) {
    // \u0041 is a VALID escape sharing a string with an invalid \U — repair
    // must double only the invalid one (over-escaping would corrupt \u0041
    // into literal text instead of "A").
    const char* body =
        "{\"0\":{\"summary\":\"A=\\u0041 path C:\\Users\",\"score\":0.5}}";
    auto r = ParseEnrichBatchResponse(body, 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].summary, "A=A path C:\\Users");
}

TEST(EnricherResponseParserTest, L3DanglingTrailingBackslashIsNotFakeRescued) {
    // A value ending in a lone backslash swallows its closing quote; intent is
    // unrecoverable and the repair must NOT fake a rescue — straight L3.
    const char* body = "{\"0\":{\"summary\":\"trail\\\",\"score\":0.5}}";
    auto r = ParseEnrichBatchResponse(body, 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].status, static_cast<int>(EnricherErrorCode::kParse));
}

TEST(EnricherResponseParserTest, L3OnJsonArrayNotObject) {
    // A JSON array parses but is not the expected object → L3.
    auto r = ParseEnrichBatchResponse("[1,2,3]", 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].status, static_cast<int>(EnricherErrorCode::kParse));
    auto sd = nlohmann::json::parse(r[0].error_meta.structured_data);
    EXPECT_EQ(sd["layer"], "L3");
}

TEST(EnricherResponseParserTest, L2ChunkMissing) {
    // index 1 absent → L2 for chunk 1, chunk 0 success.
    auto r = ParseEnrichBatchResponse(
        R"({"0":{"summary":"ok","score":0.8}})", 2, "m", "default-zh");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[1].status, static_cast<int>(EnricherErrorCode::kParse));
    auto sd = nlohmann::json::parse(r[1].error_meta.structured_data);
    EXPECT_EQ(sd["layer"], "L2");
}

TEST(EnricherResponseParserTest, L1FieldMissingDefaultsButSucceeds) {
    // chunk 0 has no summary, no entities, no score → defaults, still success.
    auto r = ParseEnrichBatchResponse(R"({"0":{}})", 1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());  // L1: missing fields are acceptable (Issue 1.6)
    EXPECT_TRUE(r[0].entities.empty());
    EXPECT_EQ(r[0].summary, "");
    EXPECT_FLOAT_EQ(r[0].enriched_score, 0.0f);
}

TEST(EnricherResponseParserTest, L1SkipsMalformedEntityElements) {
    auto r = ParseEnrichBatchResponse(
        R"({"0":{"entities":[{"text":"A","type":"ORG"}, "garbage", 42,
                {"text":"B","type":"PERSON","start_offset":3,"end_offset":4}],
            "summary":"s","score":0.5}})",
        1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());
    ASSERT_EQ(r[0].entities.size(), 2u);  // 2 valid objects; "garbage"/42 skipped
    EXPECT_EQ(r[0].entities[0].text, "A");
    EXPECT_EQ(r[0].entities[0].start_offset, 0);  // missing offsets → 0 default
    EXPECT_EQ(r[0].entities[1].text, "B");
    EXPECT_EQ(r[0].entities[1].end_offset, 4);
}

TEST(EnricherResponseParserTest, L1ToleratesStringTypedOffsets) {
    // Regression (2026-06-26 QA): a reasoning LLM (e.g. GLM-5.2) occasionally
    // emits numeric fields as JSON strings. A present string previously threw
    // type_error.302, which escaped ParseEntities (despite its "never throws"
    // contract) and dropped the ENTIRE batch's enrichment. ReadIntField now
    // recovers a numeric-string offset; the chunk stays success either way.
    auto r = ParseEnrichBatchResponse(
        R"({"0":{"entities":[{"text":"A","type":"ORG","start_offset":"5","end_offset":"9"}],
            "summary":"s","score":0.5}})",
        1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());                       // did NOT throw / drop the chunk
    ASSERT_EQ(r[0].entities.size(), 1u);
    EXPECT_EQ(r[0].entities[0].text, "A");
    EXPECT_EQ(r[0].entities[0].start_offset, 5);  // numeric string offset → recovered
    EXPECT_EQ(r[0].entities[0].end_offset, 9);
    EXPECT_EQ(r[0].summary, "s");                 // rest of the chunk survives
    EXPECT_FLOAT_EQ(r[0].enriched_score, 0.5f);
}

TEST(EnricherResponseParserTest, L1NumericStringEntityOffsetsRecover) {
    auto r = ParseEnrichBatchResponse(
        R"({"0":{"entities":[{"text":"Cortrix","type":"PRODUCT","start_offset":"0","end_offset":"7"}],
            "summary":"s","score":0.5}})",
        1, "m", "default-zh");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_TRUE(r[0].ok());
    ASSERT_EQ(r[0].entities.size(), 1u);
    EXPECT_EQ(r[0].entities[0].start_offset, 0);
    EXPECT_EQ(r[0].entities[0].end_offset, 7);
}

TEST(EnricherResponseParserTest, CompleteFencedJsonBodyParses) {
    auto r = ParseEnrichBatchResponse(
        std::string("```json\n") + kGoodBody + "\n```", 2, "m", "default-zh");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_TRUE(r[1].ok());
}

TEST(EnricherResponseParserTest, ScoreClampedIntoRange) {
    auto r = ParseEnrichBatchResponse(
        R"({"0":{"summary":"hi","score":1.7},"1":{"summary":"lo","score":-0.5}})",
        2, "m", "default-zh");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_FLOAT_EQ(r[0].enriched_score, 1.0f);
    EXPECT_FLOAT_EQ(r[1].enriched_score, 0.0f);
}

TEST(EnricherResponseParserTest, ZeroBatchSizeEmpty) {
    auto r = ParseEnrichBatchResponse(kGoodBody, 0, "m", "default-zh");
    EXPECT_TRUE(r.empty());
}

}  // namespace
}  // namespace cortrix::spc
