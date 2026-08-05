#include <gtest/gtest.h>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/agent_trace/agent_trace_error.h"
#include "cortrix/common/status.h"

// Exhaustive error-registry matrix for agent_trace (src/agent_trace/agent_trace_error.*).
// One TEST_P case per AgentTraceErrorCode. Unique suite name (AgentTraceErrorMatrix).
//
// NOTE (registry quirk, intentionally not over-asserted): AGENTTRACE_INTERNAL is
// retryable=true with retry_after_ms=null (the Agent may retry but no fixed backoff
// is promised). So only the universal direction "retry_after_ms present => retryable"
// is asserted here; the reverse does NOT hold for this module.
namespace cortrix::agent_trace {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<AgentTraceErrorCode>& AllCodes() {
    static const std::vector<AgentTraceErrorCode> codes = {
        AgentTraceErrorCode::kSessionNotFound,
        AgentTraceErrorCode::kInvalidFilter,
        AgentTraceErrorCode::kInteractionNotFound,
        AgentTraceErrorCode::kUnauthorized,
        AgentTraceErrorCode::kMcpSessionInvalid,
        AgentTraceErrorCode::kSessionExpired,
        AgentTraceErrorCode::kInternal,
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

class AgentTraceErrorMatrix : public ::testing::TestWithParam<AgentTraceErrorCode> {};

TEST_P(AgentTraceErrorMatrix, CodeStringWellFormed) {
    static const std::regex kPattern("^CX_(ERR|WARN)_TRACE_[A-Z][A-Z0-9_]*$");
    const std::string cx = AgentTraceErrorCodeString(GetParam());
    EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
    EXPECT_EQ(cx, std::string(GetAgentTraceErrorInfo(GetParam()).cx_code));
}

TEST_P(AgentTraceErrorMatrix, CategoryInValidSet) {
    const AgentTraceErrorInfo& info = GetAgentTraceErrorInfo(GetParam());
    EXPECT_EQ(ValidCategoryStrings().count(agent_friendly::ToString(info.category)), 1u)
        << "code " << info.cx_code;
}

// Universal invariant only: a present backoff implies the code is retryable. agent trace
// deliberately does NOT carry the reverse (INTERNAL is retryable with null backoff).
TEST_P(AgentTraceErrorMatrix, RetryAfterImpliesRetryable) {
    const AgentTraceErrorInfo& info = GetAgentTraceErrorInfo(GetParam());
    if (info.retry_after_ms.has_value()) {
        EXPECT_TRUE(info.retryable) << info.cx_code;
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(AgentTraceErrorMatrix, RequiredStructuredDataContract) {
    const AgentTraceErrorCode code = GetParam();
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

TEST_P(AgentTraceErrorMatrix, MakeErrorRoundTrips) {
    const AgentTraceErrorCode code = GetParam();
    const AgentTraceErrorInfo& info = GetAgentTraceErrorInfo(code);
    nlohmann::json sd = {{"probe", 1}};

    auto err = MakeAgentTraceError(code, sd, "human detail");
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
    if (info.retry_after_ms.has_value()) {
        EXPECT_EQ(body["retry_after_ms"], *info.retry_after_ms);
    } else {
        EXPECT_TRUE(body["retry_after_ms"].is_null());
    }
    EXPECT_EQ(body["structured_data"]["probe"], 1);
}

TEST_P(AgentTraceErrorMatrix, EmptyMessageFallsBackToCode) {
    const AgentTraceErrorCode code = GetParam();
    auto err = MakeAgentTraceError(code);
    EXPECT_EQ(err.message, std::string(AgentTraceErrorCodeString(code)));
}

TEST_P(AgentTraceErrorMatrix, StatusCodeBucketAndBridge) {
    const AgentTraceErrorCode code = GetParam();
    const StatusCode sc = AgentTraceErrorToStatusCode(code);
    EXPECT_EQ(ValidStatusCodes().count(sc), 1u);
    EXPECT_NE(sc, StatusCode::kOk);

    Status s = AgentTraceStatus(code, "detail-z");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), sc);
    EXPECT_NE(s.message().find(AgentTraceErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-z"), std::string::npos);

    Status bare = AgentTraceStatus(code);
    EXPECT_EQ(bare.message(), std::string(AgentTraceErrorCodeString(code)));
}

INSTANTIATE_TEST_SUITE_P(AllAgentTraceCodes, AgentTraceErrorMatrix,
                         ::testing::ValuesIn(AllCodes()));

TEST(AgentTraceErrorMatrixGuard, CodeStringsAreUniqueAndCountMatches) {
    std::set<std::string> seen;
    for (AgentTraceErrorCode code : AllCodes()) {
        EXPECT_TRUE(seen.insert(AgentTraceErrorCodeString(code)).second)
            << "duplicate: " << AgentTraceErrorCodeString(code);
    }
    EXPECT_EQ(seen.size(), AllCodes().size());
    EXPECT_EQ(static_cast<int>(AllCodes().size()), kAgentTraceErrorCodeCount);
}

}  // namespace
}  // namespace cortrix::agent_trace
