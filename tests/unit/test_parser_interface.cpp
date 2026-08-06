#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/spc/parser.h"
#include "cortrix/spc/parser_errors.h"
#include "parser_stub.h"

// Parser S1 coverage: IDocumentParser interface contract (StubParser) + the
// ParserErrorCode registry (Agent-friendly mapping). Mirrors the
// catalog_error test for the registry half.
namespace cortrix::spc {
namespace {

using agent_friendly::ErrorCategory;
using test::StubParser;

// --- IDocumentParser interface ---

TEST(ParserInterfaceTest, Interface_StubParser_Success) {
    StubParser p("stub", {"pdf", "txt"});
    p.SetResult(test::MakeOnePageDoc("stub", 0.9f));
    ParsedDoc doc = p.Parse("/tmp/x.pdf");
    EXPECT_TRUE(doc.ok());
    EXPECT_EQ(doc.status, ParserErrorCode::kOk);
    EXPECT_EQ(doc.parser_name, "stub");
    ASSERT_EQ(doc.pages.size(), 1u);
    EXPECT_EQ(doc.pages[0].paragraphs.size(), 1u);
    EXPECT_EQ(p.call_count(), 1);
}

TEST(ParserInterfaceTest, Interface_SupportedFormats) {
    StubParser p("stub", {"pdf", "docx", "md"});
    EXPECT_EQ(p.SupportedFormats(),
              (std::vector<std::string>{"pdf", "docx", "md"}));
}

TEST(ParserInterfaceTest, Interface_IsFormatSupported_Yes) {
    StubParser p("stub", {"pdf", "docx"});
    EXPECT_TRUE(p.IsFormatSupported("pdf"));
    // Case-insensitive + tolerant of a leading dot.
    EXPECT_TRUE(p.IsFormatSupported("PDF"));
    EXPECT_TRUE(p.IsFormatSupported(".docx"));
}

TEST(ParserInterfaceTest, Interface_IsFormatSupported_No) {
    StubParser p("stub", {"pdf", "docx"});
    EXPECT_FALSE(p.IsFormatSupported("exe"));
    EXPECT_FALSE(p.IsFormatSupported(""));
}

TEST(ParserInterfaceTest, ChunkTypeRoundTripsThroughString) {
    for (ChunkType t : {ChunkType::TEXT, ChunkType::TABLE, ChunkType::IMAGE_CAPTION,
                        ChunkType::CODE, ChunkType::META}) {
        EXPECT_EQ(ChunkTypeFromString(ToString(t)), t);
    }
    // Unknown string falls back to TEXT (tolerant bridge JSON parsing).
    EXPECT_EQ(ChunkTypeFromString("NONSENSE"), ChunkType::TEXT);
}

// --- ParserErrorCode registry ---

// All 16 codes, in enum order. Explicit (not a loop over ints) so the test
// documents the locked set and fails to compile if an enumerator disappears.
const std::vector<ParserErrorCode>& AllCodes() {
    static const std::vector<ParserErrorCode> codes = {
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
    return codes;
}

TEST(ParserErrorTest, SixteenCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 16u);
    EXPECT_EQ(kParserErrorCodeCount, 16);
}

// Enum integer values match the parser wire numbers (also ParsedDoc.status /
// bridge JSON status). A drift here breaks the C++↔Python contract.
TEST(ParserErrorTest, EnumValuesMatchWireProtocol) {
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kOk), 0);
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kFileNotFound), 1);
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kFileTooLarge), 2);
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kParseTimeout), 4);
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kEmptyDocument), 9);
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kPasswordProtected), 13);
    EXPECT_EQ(static_cast<int>(ParserErrorCode::kMaxPagesExceeded), 15);
}

// kOk and kEmptyDocument are the only non-error outcomes (empty doc =>
// status=OK, pages=[]).
TEST(ParserErrorTest, OnlyOkAndEmptyAreNonError) {
    for (ParserErrorCode code : AllCodes()) {
        const bool is_ok = (code == ParserErrorCode::kOk ||
                            code == ParserErrorCode::kEmptyDocument);
        EXPECT_EQ(IsParserOk(code), is_ok)
            << "code " << ParserErrorCodeString(code);
    }
}

// Every code's CX_ERR_* string is unique and matches the API spec ErrorResponseV1
// pattern ^CX_ERR_[A-Z][A-Z_]*$.
TEST(ParserErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (ParserErrorCode code : AllCodes()) {
        std::string cx = ParserErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate code string: " << cx;
    }
    EXPECT_EQ(seen.size(), 16u);
}

// retryable ⇔ retry_after_ms present, and only transient/timeout codes retry
// (mirrors the retry_after_ms column).
TEST(ParserErrorTest, RetryableIffRetryAfterPresent) {
    for (ParserErrorCode code : AllCodes()) {
        const ParserErrorInfo& info = GetParserErrorInfo(code);
        EXPECT_EQ(info.retryable, info.retry_after_ms.has_value())
            << "code " << info.cx_code << ": retryable/retry_after_ms mismatch";
        if (info.retryable) {
            EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
            EXPECT_TRUE(info.category == ErrorCategory::kTransient ||
                        info.category == ErrorCategory::kTimeout)
                << info.cx_code << ": only transient/timeout codes are retryable";
        }
    }
}

// Spot-check the exact rows downstream consumers (HTTP API / async task progress)
// depend on.
TEST(ParserErrorTest, SpecificRowsMatchSpec) {
    auto check = [](ParserErrorCode c, const char* cx, ErrorCategory cat,
                    bool retry, std::optional<int> after) {
        const ParserErrorInfo& i = GetParserErrorInfo(c);
        EXPECT_STREQ(i.cx_code, cx);
        EXPECT_EQ(i.category, cat);
        EXPECT_EQ(i.retryable, retry);
        EXPECT_EQ(i.retry_after_ms, after);
    };
    check(ParserErrorCode::kFileTooLarge, "CX_ERR_FILE_TOO_LARGE",
          ErrorCategory::kQuota, false, std::nullopt);
    check(ParserErrorCode::kParseTimeout, "CX_ERR_PARSE_TIMEOUT",
          ErrorCategory::kTimeout, true, 1000);
    check(ParserErrorCode::kSubprocessCrashed, "CX_ERR_SUBPROCESS_CRASHED",
          ErrorCategory::kTransient, true, 500);
    check(ParserErrorCode::kInvalidOutput, "CX_ERR_INVALID_OUTPUT",
          ErrorCategory::kTransient, true, 500);
    check(ParserErrorCode::kOcrFailed, "CX_ERR_OCR_FAILED",
          ErrorCategory::kTransient, true, 1000);
    check(ParserErrorCode::kPasswordProtected, "CX_ERR_PASSWORD_PROTECTED",
          ErrorCategory::kPermanent, false, std::nullopt);
    check(ParserErrorCode::kMaxPagesExceeded, "CX_ERR_MAX_PAGES_EXCEEDED",
          ErrorCategory::kQuota, false, std::nullopt);
}

// MakeParserError fills the boundary error from the registry, injects the code
// into structured_data, and ToJson yields the Agent-friendly contract body shape.
TEST(ParserErrorTest, MakeParserErrorBuildsAgentFriendlyBody) {
    nlohmann::json sd = {{"actual_size_mb", 350}, {"limit_mb", 200},
                         {"tier", "ce"}};
    auto err = MakeParserError(ParserErrorCode::kFileTooLarge, sd);
    EXPECT_EQ(err.code, "CX_ERR_FILE_TOO_LARGE");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kQuota);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["code"], "CX_ERR_FILE_TOO_LARGE");
    EXPECT_EQ((*err.structured_data)["actual_size_mb"], 350);
    // default message falls back to the code string.
    EXPECT_EQ(err.message, "CX_ERR_FILE_TOO_LARGE");

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_FILE_TOO_LARGE");
    EXPECT_EQ(body["category"], "quota");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
    EXPECT_EQ(body["structured_data"]["limit_mb"], 200);
}

// A retryable code surfaces retry_after_ms through the JSON body for the Agent.
TEST(ParserErrorTest, RetryableCodeExposesRetryAfterInJson) {
    auto err = MakeParserError(ParserErrorCode::kParseTimeout,
                               {{"timeout_ms", 300000},
                                {"pages_completed_before_timeout", 47}},
                               "Subprocess timeout");
    EXPECT_TRUE(err.retryable);
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["retryable"], true);
    EXPECT_EQ(body["retry_after_ms"], 1000);
    EXPECT_EQ(body["category"], "timeout");
    EXPECT_EQ(body["message"], "Subprocess timeout");
}

// Required structured_data keys match and the completeness check works.
TEST(ParserErrorTest, RequiredStructuredDataKeysEnforced) {
    // FILE_TOO_LARGE requires actual_size_mb / limit_mb / tier.
    nlohmann::json complete = {{"code", "CX_ERR_FILE_TOO_LARGE"},
                               {"actual_size_mb", 350}, {"limit_mb", 200},
                               {"tier", "ce"}};
    EXPECT_TRUE(HasRequiredParserStructuredData(ParserErrorCode::kFileTooLarge, complete));

    nlohmann::json missing = {{"code", "CX_ERR_FILE_TOO_LARGE"},
                              {"actual_size_mb", 350}};
    EXPECT_FALSE(HasRequiredParserStructuredData(ParserErrorCode::kFileTooLarge, missing));

    // Non-object payload is never complete.
    EXPECT_FALSE(HasRequiredParserStructuredData(ParserErrorCode::kFileTooLarge,
                                                 nlohmann::json::array()));
    // kOk / kEmptyDocument require nothing.
    EXPECT_TRUE(HasRequiredParserStructuredData(ParserErrorCode::kEmptyDocument,
                                                nlohmann::json::object()));
}

// ParserStatus bridges to a plain Status carrying the CX_ERR_* token, and Ok
// for the non-error codes (F-FREEZE-1 single error model).
TEST(ParserErrorTest, ParserStatusBridgesToStatus) {
    EXPECT_TRUE(ParserStatus(ParserErrorCode::kOk).ok());
    EXPECT_TRUE(ParserStatus(ParserErrorCode::kEmptyDocument).ok());

    Status s = ParserStatus(ParserErrorCode::kFileNotFound, "/tmp/x.pdf");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_FILE_NOT_FOUND"), std::string::npos);
    EXPECT_NE(s.message().find("/tmp/x.pdf"), std::string::npos);

    EXPECT_EQ(ParserErrorToStatusCode(ParserErrorCode::kParseTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(ParserErrorToStatusCode(ParserErrorCode::kInvalidOutput),
              StatusCode::kInternal);
}

// MakeAgentFriendlyError reconstructs the body from a populated ParsedDoc.
TEST(ParserErrorTest, MakeAgentFriendlyErrorFromParsedDoc) {
    ParsedDoc doc;
    doc.status = ParserErrorCode::kSubprocessCrashed;
    doc.error_msg = "boom";
    doc.structured_data = {{"exit_code", 139}, {"stderr_tail", "segfault"}};
    auto err = MakeAgentFriendlyError(doc);
    EXPECT_EQ(err.code, "CX_ERR_SUBPROCESS_CRASHED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    EXPECT_EQ(err.retry_after_ms, 500);
    EXPECT_EQ(err.message, "boom");
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["exit_code"], 139);
}

}  // namespace
}  // namespace cortrix::spc
