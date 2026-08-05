#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/metadata/metadata_error.h"

// META block S1.2 / §5.1 coverage: the 3 metadata error identities — CX_ERR_*/CX_WARN_*
// identity, category mapping, retryability, the GEN-Agent 4-field boundary factory,
// the §5.1.1 structured_data contract, and the Status bridge. Mirrors the project
// reference test tests/unit/test_import_error.cpp (template A).
namespace cortrix::metadata {
namespace {

using agent_friendly::ErrorCategory;

// All 3 codes, in enum order. Explicit (not a loop over ints) so the test itself
// documents the locked set and fails to compile if an enumerator is gone.
const std::vector<MetadataErrorCode>& AllCodes() {
    static const std::vector<MetadataErrorCode> codes = {
        MetadataErrorCode::kGenFailed,
        MetadataErrorCode::kPartial,
        MetadataErrorCode::kFieldImmutable,
    };
    return codes;
}

TEST(MetadataErrorTest, ThreeCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 3u);
    EXPECT_EQ(kMetadataErrorCodeCount, 3);
}

// Every code's stable string is unique and matches the API spec pattern (GEN-Agent #1 +
// #7 stable identity). Errors are CX_ERR_*, the warning is CX_WARN_*.
TEST(MetadataErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_(ERR|WARN)_METADATA_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (MetadataErrorCode code : AllCodes()) {
        std::string cx = MetadataErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate " << cx;
    }
    EXPECT_EQ(seen.size(), 3u);
    // The warning code uses the CX_WARN_ prefix, the two errors CX_ERR_.
    EXPECT_EQ(std::string(MetadataErrorCodeString(MetadataErrorCode::kPartial)),
              "CX_WARN_METADATA_PARTIAL");
    EXPECT_EQ(std::string(MetadataErrorCodeString(MetadataErrorCode::kGenFailed)),
              "CX_ERR_METADATA_GEN_FAILED");
    EXPECT_EQ(std::string(MetadataErrorCodeString(MetadataErrorCode::kFieldImmutable)),
              "CX_ERR_METADATA_FIELD_IMMUTABLE");
}

// §5.1 column values pinned exactly (HTTP / category / retryable).
TEST(MetadataErrorTest, RegistryMatchesSpecTable) {
    auto info = [](MetadataErrorCode c) { return GetMetadataErrorInfo(c); };

    EXPECT_EQ(info(MetadataErrorCode::kGenFailed).http_status, 500);
    EXPECT_EQ(info(MetadataErrorCode::kGenFailed).category, ErrorCategory::kPermanent);
    EXPECT_FALSE(info(MetadataErrorCode::kGenFailed).retryable);

    EXPECT_EQ(info(MetadataErrorCode::kPartial).http_status, 200);
    EXPECT_EQ(info(MetadataErrorCode::kPartial).category, ErrorCategory::kPermanent);
    EXPECT_FALSE(info(MetadataErrorCode::kPartial).retryable);

    EXPECT_EQ(info(MetadataErrorCode::kFieldImmutable).http_status, 405);
    EXPECT_EQ(info(MetadataErrorCode::kFieldImmutable).category, ErrorCategory::kPermanent);
    EXPECT_FALSE(info(MetadataErrorCode::kFieldImmutable).retryable);
}

// retry_after_ms is present iff retryable (GEN-Agent #6). All 3 codes are
// non-retryable → all have null retry_after_ms (§5.1).
TEST(MetadataErrorTest, RetryAfterMsConsistentWithRetryable) {
    for (MetadataErrorCode code : AllCodes()) {
        const MetadataErrorInfo& info = GetMetadataErrorInfo(code);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value())
            << MetadataErrorCodeString(code);
        EXPECT_FALSE(info.retry_after_ms.has_value());
    }
}

// MakeMetadataError fills the 4 GEN-Agent fields from the registry (call sites never
// restate category/retryable/retry_after_ms).
TEST(MetadataErrorTest, MakeErrorPopulatesAgentFriendlyFields) {
    auto err = MakeMetadataError(
        MetadataErrorCode::kGenFailed,
        {{"doc_id", "doc_x"},
         {"namespace_id", "default"},
         {"stage", "block_text_assembly"},
         {"missing_fields", {"doc_metadata"}},
         {"f06_parse_status", "failed"},
         {"downstream_blocked", {"f34_chunker"}}},
        "Failed to generate metadata block: doc_metadata is empty");
    EXPECT_EQ(err.code, "CX_ERR_METADATA_GEN_FAILED");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["f06_parse_status"], "failed");

    // Serializes to the §3.1 error body shape (retry_after_ms → JSON null).
    auto body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_METADATA_GEN_FAILED");
    EXPECT_EQ(body["category"], "permanent");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
    EXPECT_TRUE(body["structured_data"].is_object());
}

// Empty message falls back to the CX_ token (never an empty string).
TEST(MetadataErrorTest, EmptyMessageFallsBackToCode) {
    auto err = MakeMetadataError(MetadataErrorCode::kFieldImmutable);
    EXPECT_EQ(err.message, "CX_ERR_METADATA_FIELD_IMMUTABLE");
}

// Required structured_data keys (GEN-Agent #5, §5.1.1 example bodies).
TEST(MetadataErrorTest, RequiredStructuredDataContract) {
    // PARTIAL's required top-level keys are contextual (warning body built by the
    // response layer's meta.warnings[]).
    EXPECT_TRUE(RequiredStructuredDataKeys(MetadataErrorCode::kPartial).empty());

    // GEN_FAILED full body (§5.1.1 example A).
    nlohmann::json gen_full = {
        {"doc_id", "d"}, {"namespace_id", "n"}, {"stage", "s"},
        {"missing_fields", nlohmann::json::array()},
        {"f06_parse_status", "failed"},
        {"downstream_blocked", nlohmann::json::array()}};
    EXPECT_TRUE(HasRequiredStructuredData(MetadataErrorCode::kGenFailed, gen_full));
    nlohmann::json gen_partial = {{"doc_id", "d"}};
    EXPECT_FALSE(HasRequiredStructuredData(MetadataErrorCode::kGenFailed, gen_partial));

    // FIELD_IMMUTABLE full body (§5.1.1 example B).
    nlohmann::json imm_full = {
        {"doc_id", "d"}, {"namespace_id", "n"},
        {"attempted_fields", nlohmann::json::array()},
        {"v1_immutable_fields", nlohmann::json::array()},
        {"phase2_feature", "F08 Block Versioning"},
        {"workaround", "re-upload"}};
    EXPECT_TRUE(HasRequiredStructuredData(MetadataErrorCode::kFieldImmutable, imm_full));
    EXPECT_FALSE(HasRequiredStructuredData(MetadataErrorCode::kFieldImmutable,
                                           nlohmann::json{{"doc_id", "d"}}));

    // A non-object structured_data satisfies only an empty key list.
    EXPECT_TRUE(HasRequiredStructuredData(MetadataErrorCode::kPartial, nlohmann::json("x")));
    EXPECT_FALSE(HasRequiredStructuredData(MetadataErrorCode::kGenFailed, nlohmann::json("x")));
}

// MetadataStatus bridges to a coarse Status whose message recovers the exact identity
// (F-FREEZE-1: no Result<T,E>; identity travels in the CX_ prefix).
TEST(MetadataErrorTest, StatusBridgePreservesIdentityInMessage) {
    Status s = MetadataStatus(MetadataErrorCode::kGenFailed, "doc_metadata empty");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInternal);
    EXPECT_NE(s.message().find("CX_ERR_METADATA_GEN_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("doc_metadata empty"), std::string::npos);

    EXPECT_EQ(MetadataStatus(MetadataErrorCode::kFieldImmutable).code(),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(MetadataStatus(MetadataErrorCode::kPartial).code(),
              StatusCode::kInvalidArgument);
    // Empty detail → message is exactly the CX token.
    EXPECT_EQ(MetadataStatus(MetadataErrorCode::kGenFailed).message(),
              "CX_ERR_METADATA_GEN_FAILED");
}

}  // namespace
}  // namespace cortrix::metadata
