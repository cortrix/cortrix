#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/parser_errors.h"

// Exhaustive parameterized error-registry sweep for F06 document-parser (§5.2, 16
// codes incl. the two non-error outcomes kOk / kEmptyDocument). Distinct suite
// name (ParserErrorMatrix) from the existing parser tests. Parser uses its own
// helper names: IsParserOk / RequiredParserStructuredDataKeys /
// HasRequiredParserStructuredData. MakeParserError injects structured_data["code"].
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

const ParserErrorCode kAll[] = {
    ParserErrorCode::kOk,
    ParserErrorCode::kFileNotFound,
    ParserErrorCode::kFileTooLarge,
    ParserErrorCode::kUnsupportedFormat,
    ParserErrorCode::kParseTimeout,
    ParserErrorCode::kSubprocessFailed,
    ParserErrorCode::kSubprocessCrashed,
    ParserErrorCode::kInvalidOutput,
    ParserErrorCode::kOutputTooLarge,
    ParserErrorCode::kEmptyDocument,
    ParserErrorCode::kEncodingError,
    ParserErrorCode::kOcrFailed,
    ParserErrorCode::kAllParsersFailed,
    ParserErrorCode::kPasswordProtected,
    ParserErrorCode::kCorruptedFile,
    ParserErrorCode::kMaxPagesExceeded,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class ParserErrorMatrix : public ::testing::TestWithParam<ParserErrorCode> {};

TEST_P(ParserErrorMatrix, CodeStringFormat) {
    const ParserErrorCode code = GetParam();
    const std::string s = ParserErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetParserErrorInfo(code).cx_code, s.c_str());
}

TEST_P(ParserErrorMatrix, CategoryAndRetryInvariant) {
    const ParserErrorCode code = GetParam();
    const ParserErrorInfo& info = GetParserErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(ParserErrorMatrix, RequiredStructuredDataArms) {
    const ParserErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredParserStructuredDataKeys(code);

    // All-present object passes.
    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredParserStructuredData(code, full));

    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredParserStructuredData(code, missing));
    }

    // Parser's Has* returns false for any non-object payload (unlike the other
    // registries which pass non-object when no keys are required).
    EXPECT_FALSE(HasRequiredParserStructuredData(code, nlohmann::json("not-an-object")));
}

TEST_P(ParserErrorMatrix, MakeRoundTripAndJson) {
    const ParserErrorCode code = GetParam();
    if (IsParserOk(code)) return;  // Make precondition: error codes only.
    const ParserErrorInfo& info = GetParserErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    // Parser's required-keys list includes "code"; leave it unset so MakeParserError
    // injects the CX_ERR_* identity (it only fills "code" when absent — it never
    // overwrites a caller-supplied value).
    for (const std::string& k : RequiredParserStructuredDataKeys(code))
        if (k != "code") sd[k] = "v";

    auto err = MakeParserError(code, sd, "human detail");
    EXPECT_EQ(err.code, info.cx_code);
    EXPECT_EQ(err.message, "human detail");
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);
    // Make injects the CX_ERR_* identity into structured_data["code"].
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["code"], std::string(info.cx_code));

    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], std::string(info.cx_code));
    EXPECT_EQ(j["retryable"], info.retryable);
    EXPECT_EQ(j["category"], std::string(agent_friendly::ToString(info.category)));
    EXPECT_EQ(j["retry_after_ms"].is_null(), !info.retry_after_ms.has_value());
}

TEST_P(ParserErrorMatrix, EmptyMessageFallsBackToCode) {
    const ParserErrorCode code = GetParam();
    if (IsParserOk(code)) return;
    auto err = MakeParserError(code);
    EXPECT_EQ(err.message, std::string(ParserErrorCodeString(code)));
}

TEST_P(ParserErrorMatrix, StatusBridge) {
    const ParserErrorCode code = GetParam();
    const StatusCode mapped = ParserErrorToStatusCode(code);
    Status s = ParserStatus(code, "detail-x");

    if (IsParserOk(code)) {
        // kOk / kEmptyDocument map to kOk and produce an Ok status.
        EXPECT_EQ(mapped, StatusCode::kOk);
        EXPECT_TRUE(s.ok());
        return;
    }

    EXPECT_NE(mapped, StatusCode::kOk);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(ParserErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(ParserErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kParserErrorCodeCount, 16);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kParserErrorCodeCount));
    std::set<std::string> seen;
    for (ParserErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(ParserErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kParserErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, ParserErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::spc
