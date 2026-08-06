#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/chunker/chunker_agent_error.h"
#include "cortrix/chunker/chunker_errors.h"
#include "cortrix/chunker/chunker_response_meta.h"

namespace cortrix::chunker {
namespace {

using agent_friendly::ErrorCategory;

// --- error registry --------------------------------------------

TEST(ChunkerAgentErrorTest, CodeStringsMatchSpec) {
    EXPECT_STREQ(ChunkerErrorCodeString(ChunkerErrorCode::kSizeInvalid), "CX_ERR_CHUNK_SIZE_INVALID");
    EXPECT_STREQ(ChunkerErrorCodeString(ChunkerErrorCode::kEmptyDocument), "CX_ERR_CHUNK_EMPTY_DOCUMENT");
    EXPECT_STREQ(ChunkerErrorCodeString(ChunkerErrorCode::kOverflow), "CX_ERR_CHUNK_OVERFLOW");
}

TEST(ChunkerAgentErrorTest, AllHardErrorsArePermanentNonRetryable) {
    for (auto code : {ChunkerErrorCode::kSizeInvalid, ChunkerErrorCode::kEmptyDocument,
                      ChunkerErrorCode::kOverflow}) {
        const auto& info = GetChunkerErrorInfo(code);
        EXPECT_EQ(info.category, ErrorCategory::kPermanent);
        EXPECT_FALSE(info.retryable);
        EXPECT_FALSE(info.retry_after_ms.has_value());
    }
}

TEST(ChunkerAgentErrorTest, CodeCountIsThree) {
    EXPECT_EQ(kChunkerErrorCodeCount, 3);
}

TEST(ChunkerAgentErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(ChunkerErrorCode::kSizeInvalid),
              (std::vector<std::string>{"child_size", "max_seq_length"}));
    EXPECT_EQ(RequiredStructuredDataKeys(ChunkerErrorCode::kEmptyDocument),
              (std::vector<std::string>{"doc_id", "total_pages", "failed_pages"}));
    EXPECT_EQ(RequiredStructuredDataKeys(ChunkerErrorCode::kOverflow),
              (std::vector<std::string>{"doc_id", "estimated_parent_count",
                                        "max_parents_per_doc", "fallback_attempted"}));
}

TEST(ChunkerAgentErrorTest, HasRequiredStructuredData) {
    nlohmann::json good = {{"child_size", 600}, {"max_seq_length", 512}};
    EXPECT_TRUE(HasRequiredStructuredData(ChunkerErrorCode::kSizeInvalid, good));
    nlohmann::json missing = {{"child_size", 600}};
    EXPECT_FALSE(HasRequiredStructuredData(ChunkerErrorCode::kSizeInvalid, missing));
}

TEST(ChunkerAgentErrorTest, HasRequiredStructuredDataNonObjectIsFalseWhenKeysRequired) {
    // A non-object structured_data fails the required-keys check (the codes all
    // require keys, so a null/array payload is incomplete).
    nlohmann::json not_obj = nlohmann::json::array();
    EXPECT_FALSE(HasRequiredStructuredData(ChunkerErrorCode::kSizeInvalid, not_obj));
    nlohmann::json null_val;  // null
    EXPECT_FALSE(HasRequiredStructuredData(ChunkerErrorCode::kEmptyDocument, null_val));
}

TEST(ChunkerAgentErrorTest, MakeChunkerErrorEmptyMessageDefaultsToCode) {
    auto err = MakeChunkerError(ChunkerErrorCode::kSizeInvalid);  // no message
    EXPECT_EQ(err.message, "CX_ERR_CHUNK_SIZE_INVALID");
}

TEST(ChunkerAgentErrorTest, MakeChunkWarningEmptyMessageDefaultsToCode) {
    auto w = MakeChunkWarning(ChunkerWarningCode::kTruncated);  // no message / data
    EXPECT_EQ(w["message"], "CX_WARN_CHUNK_TRUNCATED");
}

TEST(ChunkerAgentErrorTest, TruncatedWarningCarriesStructuredData) {
    auto w = MakeChunkWarning(
        ChunkerWarningCode::kTruncated,
        {{"child_id", "C1"}, {"chunk_size", 600}, {"max_seq_length", 512}},
        "child exceeds reranker max_seq_length at runtime");
    EXPECT_EQ(w["code"], "CX_WARN_CHUNK_TRUNCATED");
    EXPECT_EQ(w["structured_data"]["child_id"], "C1");
}

TEST(ChunkerAgentErrorTest, MakeChunkerErrorBuildsGenAgentBody) {
    auto err = MakeChunkerError(
        ChunkerErrorCode::kOverflow,
        {{"doc_id", "D1"}, {"estimated_parent_count", 12500},
         {"max_parents_per_doc", 10000}, {"fallback_attempted", true}},
        "Document exceeded max_parents_per_doc and flat fallback failed");
    EXPECT_EQ(err.code, "CX_ERR_CHUNK_OVERFLOW");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["estimated_parent_count"], 12500);

    // Serializes to the Agent-friendly contract body.
    auto j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_CHUNK_OVERFLOW");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
}

TEST(ChunkerAgentErrorTest, StatusCodeMappingAndBridge) {
    EXPECT_EQ(ChunkerErrorToStatusCode(ChunkerErrorCode::kSizeInvalid), StatusCode::kInvalidArgument);
    EXPECT_EQ(ChunkerErrorToStatusCode(ChunkerErrorCode::kEmptyDocument), StatusCode::kInvalidArgument);
    EXPECT_EQ(ChunkerErrorToStatusCode(ChunkerErrorCode::kOverflow), StatusCode::kInvalidArgument);

    Status s = ChunkerErrorStatus(ChunkerErrorCode::kEmptyDocument, "no pages");
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_CHUNK_EMPTY_DOCUMENT"), std::string::npos);
    EXPECT_NE(s.message().find("no pages"), std::string::npos);
}

// --- warnings (rows 4-5) -----------------------------------------

TEST(ChunkerAgentErrorTest, WarningCodeStrings) {
    EXPECT_STREQ(ChunkerWarningCodeString(ChunkerWarningCode::kTruncated), "CX_WARN_CHUNK_TRUNCATED");
    EXPECT_STREQ(ChunkerWarningCodeString(ChunkerWarningCode::kFallback), "CX_WARN_CHUNK_FALLBACK");
}

TEST(ChunkerAgentErrorTest, MakeChunkWarningShape) {
    auto w = MakeChunkWarning(ChunkerWarningCode::kFallback,
                              {{"doc_id", "D1"}, {"parent_count", 12500}, {"threshold", 10000}});
    EXPECT_EQ(w["code"], "CX_WARN_CHUNK_FALLBACK");
    EXPECT_EQ(w["structured_data"]["parent_count"], 12500);
    EXPECT_TRUE(w.contains("message"));
}

// --- response meta ---------------------------------------------

TEST(ChunkerResponseMetaTest, StatsMetaShape) {
    ChunkerStats stats;
    stats.total_pages = 120;
    stats.succeeded_pages = 118;
    stats.failed_pages = 2;
    stats.total_parents = 45;
    stats.total_children = 180;
    auto j = BuildStatsMeta(stats);
    EXPECT_EQ(j["total_pages"], 120u);
    EXPECT_EQ(j["succeeded_pages"], 118u);
    EXPECT_EQ(j["failed_pages"], 2u);
    EXPECT_EQ(j["total_parents"], 45u);
    EXPECT_EQ(j["total_children"], 180u);
    EXPECT_NEAR(j["coverage_ratio"].get<double>(), 118.0 / 120.0, 1e-9);
}

TEST(ChunkerResponseMetaTest, NoWarningsWhenNoFallback) {
    ChunkerStats stats;  // fallback_to_flat = false
    auto w = BuildWarningsMeta(stats, "D1", 5, 10000);
    EXPECT_TRUE(w.is_array());
    EXPECT_TRUE(w.empty());
}

TEST(ChunkerResponseMetaTest, FallbackWarningEmitted) {
    ChunkerStats stats;
    stats.fallback_to_flat = true;
    auto w = BuildWarningsMeta(stats, "D1", 12500, 10000);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0]["code"], "CX_WARN_CHUNK_FALLBACK");
    EXPECT_EQ(w[0]["structured_data"]["doc_id"], "D1");
    EXPECT_EQ(w[0]["structured_data"]["parent_count"], 12500);
    EXPECT_EQ(w[0]["structured_data"]["threshold"], 10000);
}

TEST(ChunkerResponseMetaTest, ChildrenHitsMetaJsonShape) {
    std::vector<ChildHit> hits = {
        ChildHit{"C3", "P1", 0.7f}, ChildHit{"C7", "P1", 0.9f}, ChildHit{"C12", "P1", 0.8f}};
    auto groups = DedupChildrenToParents(hits, 3);
    auto j = BuildChildrenHitsMetaJson(groups);
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["parent_id"], "P1");
    EXPECT_EQ(j[0]["primary_child"], "C7");
    EXPECT_EQ(j[0]["hits"], (std::vector<std::string>{"C7", "C12", "C3"}));
}

}  // namespace
}  // namespace cortrix::chunker
