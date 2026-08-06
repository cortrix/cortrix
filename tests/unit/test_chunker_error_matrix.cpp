#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/chunker/chunker_agent_error.h"
#include "cortrix/common/status.h"

// Exhaustive error-registry matrix for parent-child chunker (src/chunker/chunker_agent_error.*).
// One TEST_P case per hard ChunkerErrorCode. Unique suite name (ChunkerErrorMatrix).
// The 2 non-blocking warnings (CX_WARN_CHUNK_*) are covered by a separate guard test.
namespace cortrix::chunker {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<ChunkerErrorCode>& AllCodes() {
    static const std::vector<ChunkerErrorCode> codes = {
        ChunkerErrorCode::kSizeInvalid,
        ChunkerErrorCode::kEmptyDocument,
        ChunkerErrorCode::kOverflow,
    };
    return codes;
}

const std::set<std::string>& ValidCategoryStrings() {
    static const std::set<std::string> kFive = {"auth", "quota", "transient",
                                                "permanent", "timeout"};
    return kFive;
}

const std::set<StatusCode>& ValidStatusCodes() {
    static const std::set<StatusCode> kCodes = {
        StatusCode::kInvalidArgument, StatusCode::kNotFound,
        StatusCode::kAlreadyExists,   StatusCode::kPermissionDenied,
        StatusCode::kUnauthenticated, StatusCode::kInternal,
        StatusCode::kUnavailable};
    return kCodes;
}

class ChunkerErrorMatrix : public ::testing::TestWithParam<ChunkerErrorCode> {};

// Hard error codes are CX_ERR_CHUNK_*.
TEST_P(ChunkerErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_ERR_CHUNK_[A-Z][A-Z0-9_]*$");
    const std::string cx = ChunkerErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    EXPECT_EQ(cx, std::string(GetChunkerErrorInfo(GetParam()).cx_code));
}

TEST_P(ChunkerErrorMatrix, CategoryInValidSet) {
    const ChunkerErrorInfo& info = GetChunkerErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code;
}

// All 3 hard chunker errors are permanent / non-retryable.
TEST_P(ChunkerErrorMatrix, RetryAfterImpliesRetryable) {
    const ChunkerErrorInfo& info = GetChunkerErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
    EXPECT_EQ(info.retryable, info.retry_after_ms.has_value()) << info.cx_code;
    EXPECT_FALSE(info.retryable) << info.cx_code;
}

TEST_P(ChunkerErrorMatrix, RequiredStructuredDataContract) {
    const ChunkerErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);

    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredStructuredData(code, full));

    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredStructuredData(code, missing)) << keys.front();
    }

    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json("x")), keys.empty());
    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json(nullptr)), keys.empty());
}

TEST_P(ChunkerErrorMatrix, MakeErrorRoundTrips) {
    const ChunkerErrorCode code = GetParam();
    const ChunkerErrorInfo& info = GetChunkerErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeChunkerError(code, sd, "human detail");
    EXPECT_EQ(err.code, std::string(info.cx_code));
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);
    EXPECT_EQ(err.message, "human detail");
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["probe"], 1);

    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], std::string(info.cx_code));
    EXPECT_EQ(body["category"], agent_friendly::ToString(info.category));
    EXPECT_EQ(body["retryable"], info.retryable);
    EXPECT_TRUE(body["retry_after_ms"].is_null());  // none retryable
    EXPECT_EQ(body["structured_data"]["probe"], 1);
}

TEST_P(ChunkerErrorMatrix, EmptyMessageFallsBackToCode) {
    const ChunkerErrorCode code = GetParam();
    auto err = MakeChunkerError(code);
    EXPECT_EQ(err.message, std::string(ChunkerErrorCodeString(code)));
}

TEST_P(ChunkerErrorMatrix, StatusCodeBucketAndBridge) {
    const ChunkerErrorCode code = GetParam();
    const StatusCode sc = ChunkerErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = ChunkerErrorStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(ChunkerErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    Status bare = ChunkerErrorStatus(code);
    EXPECT_EQ(bare.message(), std::string(ChunkerErrorCodeString(code)));
}

INSTANTIATE_TEST_SUITE_P(AllChunkerCodes, ChunkerErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

TEST(ChunkerErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (ChunkerErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(ChunkerErrorCodeString(code)).second)
            << "duplicate: " << ChunkerErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kChunkerErrorCodeCount);
}

// The 2 non-blocking warnings have well-formed, unique CX_WARN_CHUNK_* tokens.
TEST(ChunkerErrorMatrixGuard, WarningCodeStringsWellFormedAndUnique) {
    static const std::regex kPattern("^CX_WARN_CHUNK_[A-Z][A-Z0-9_]*$");
    const std::vector<ChunkerWarningCode> warns = {ChunkerWarningCode::kTruncated,
                                                   ChunkerWarningCode::kFallback};
    std::set<std::string> seen;
    for (ChunkerWarningCode w : warns) {
        const std::string cx = ChunkerWarningCodeString(w);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate: " << cx;
    }
    EXPECT_EQ(seen.size(), warns.size());
}

}  // namespace
}  // namespace cortrix::chunker
