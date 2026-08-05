#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_errors.h"

// Parser (Major-2, design S6): every ParserError maps to a ParsedDoc /
// AgentFriendlyError carrying the GEN-Agent 4 fields (retryable / category /
// retry_after_ms / structured_data) with the right values, and the error body
// serializes to the AGENT_FRIENDLY §3.1 shape.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;

// All 14 *error* codes (kOk / kEmptyDocument are non-error → excluded).
const std::vector<ParserErrorCode>& AllErrorCodes() {
    static const std::vector<ParserErrorCode> codes = {
        ParserErrorCode::kFileNotFound,
        ParserErrorCode::kFileTooLarge,
        ParserErrorCode::kUnsupportedFormat,
        ParserErrorCode::kParseTimeout,
        ParserErrorCode::kSubprocessFailed,
        ParserErrorCode::kSubprocessCrashed,
        ParserErrorCode::kInvalidOutput,
        ParserErrorCode::kOutputTooLarge,
        ParserErrorCode::kEncodingError,
        ParserErrorCode::kOcrFailed,
        ParserErrorCode::kAllParsersFailed,
        ParserErrorCode::kPasswordProtected,
        ParserErrorCode::kCorruptedFile,
        ParserErrorCode::kMaxPagesExceeded,
    };
    return codes;
}

// Every error code yields a non-ok ParsedDoc whose 4 Agent-friendly fields match
// the registry, and whose body serializes to the §3.1 shape with all keys.
TEST(ParserAgentFriendlyTest, EveryErrorCodeFillsFourFields) {
    for (ParserErrorCode code : AllErrorCodes()) {
        const ParserErrorInfo& info = GetParserErrorInfo(code);
        ParsedDoc doc = MakeErrorDoc(code, "detail");
        EXPECT_FALSE(doc.ok()) << info.cx_code;
        EXPECT_EQ(doc.status, code);
        // 4 fields agree with the canonical registry.
        EXPECT_EQ(doc.retryable, info.retryable) << info.cx_code;
        EXPECT_EQ(doc.category, info.category) << info.cx_code;
        EXPECT_EQ(doc.retry_after_ms, info.retry_after_ms.value_or(0)) << info.cx_code;
        // code is injected into structured_data.
        ASSERT_TRUE(doc.structured_data.is_object()) << info.cx_code;
        EXPECT_EQ(doc.structured_data["code"], info.cx_code);

        // Body round-trips to the AGENT_FRIENDLY §3.1 shape.
        nlohmann::json body = agent_friendly::ToJson(MakeAgentFriendlyError(doc));
        EXPECT_EQ(body["code"], info.cx_code);
        EXPECT_EQ(body["retryable"], info.retryable);
        EXPECT_TRUE(body.contains("category"));
        EXPECT_TRUE(body.contains("retry_after_ms"));
        EXPECT_TRUE(body.contains("structured_data"));
        if (info.retryable) {
            EXPECT_EQ(body["retry_after_ms"], *info.retry_after_ms) << info.cx_code;
        } else {
            EXPECT_TRUE(body["retry_after_ms"].is_null()) << info.cx_code;
        }
    }
}

// Spot-check the §5.2 rows the design lists as the canonical examples.
TEST(ParserAgentFriendlyTest, FileTooLarge_QuotaStructuredData) {
    ParsedDoc doc = MakeErrorDoc(
        ParserErrorCode::kFileTooLarge, "File size exceeds limit",
        {{"actual_size_mb", 350}, {"limit_mb", 200},
         {"tier", "ce"}});
    EXPECT_FALSE(doc.retryable);
    EXPECT_EQ(doc.category, ErrorCategory::kQuota);
    EXPECT_EQ(doc.retry_after_ms, 0);
    EXPECT_EQ(doc.structured_data["actual_size_mb"], 350);
    EXPECT_EQ(doc.structured_data["limit_mb"], 200);
    EXPECT_TRUE(HasRequiredParserStructuredData(ParserErrorCode::kFileTooLarge,
                                                doc.structured_data));
}

TEST(ParserAgentFriendlyTest, ParseTimeout_RetryableTimeout) {
    ParsedDoc doc = MakeErrorDoc(
        ParserErrorCode::kParseTimeout, "Subprocess timeout",
        {{"timeout_ms", 300000}, {"pages_completed_before_timeout", 47}});
    EXPECT_TRUE(doc.retryable);
    EXPECT_EQ(doc.category, ErrorCategory::kTimeout);
    EXPECT_EQ(doc.retry_after_ms, 1000);
}

TEST(ParserAgentFriendlyTest, SubprocessCrashed_TransientWithBackoff) {
    ParsedDoc doc = MakeErrorDoc(
        ParserErrorCode::kSubprocessCrashed, "crash",
        {{"exit_code", 139}, {"stderr_tail", "segfault"}});
    EXPECT_TRUE(doc.retryable);
    EXPECT_EQ(doc.category, ErrorCategory::kTransient);
    EXPECT_EQ(doc.retry_after_ms, 500);
}

TEST(ParserAgentFriendlyTest, PasswordProtected_PermanentWithHint) {
    ParsedDoc doc = MakeErrorDoc(
        ParserErrorCode::kPasswordProtected, "encrypted",
        {{"hint", "Please provide the password or upload an unencrypted version"}});
    EXPECT_FALSE(doc.retryable);
    EXPECT_EQ(doc.category, ErrorCategory::kPermanent);
    EXPECT_TRUE(HasRequiredParserStructuredData(ParserErrorCode::kPasswordProtected,
                                                doc.structured_data));
}

// Required-structured-data lists cover the keys the §5.2 examples show, for
// every code that mandates them.
TEST(ParserAgentFriendlyTest, RequiredKeysPresentForRepresentativeCodes) {
    auto keys = [](ParserErrorCode c) {
        return RequiredParserStructuredDataKeys(c);
    };
    auto contains = [](const std::vector<std::string>& v, const std::string& k) {
        for (const auto& x : v) if (x == k) return true;
        return false;
    };
    EXPECT_TRUE(contains(keys(ParserErrorCode::kUnsupportedFormat), "supported_formats"));
    EXPECT_TRUE(contains(keys(ParserErrorCode::kOcrFailed), "failed_pages"));
    EXPECT_TRUE(contains(keys(ParserErrorCode::kAllParsersFailed), "attempted"));
    EXPECT_TRUE(contains(keys(ParserErrorCode::kCorruptedFile), "magic_bytes_expected"));
    EXPECT_TRUE(contains(keys(ParserErrorCode::kMaxPagesExceeded), "max_pages"));
}

}  // namespace
}  // namespace cortrix::spc
