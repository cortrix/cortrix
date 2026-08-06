#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/agent_trace/agent_trace_error.h"

// S7 coverage: the agent trace error model (template A) — all 7 CX_ERR_TRACE_* identities,
// their attributes (category/retryable/retry_after_ms/structured_data keys),
// the AgentFriendlyError builder, the JSON body, and the Status bridge.
namespace cortrix::agent_trace {
namespace {

using agent_friendly::ErrorCategory;

constexpr AgentTraceErrorCode kAll[] = {
    AgentTraceErrorCode::kSessionNotFound,
    AgentTraceErrorCode::kInvalidFilter,
    AgentTraceErrorCode::kInteractionNotFound,
    AgentTraceErrorCode::kUnauthorized,
    AgentTraceErrorCode::kMcpSessionInvalid,
    AgentTraceErrorCode::kSessionExpired,
    AgentTraceErrorCode::kInternal,
};

TEST(AgentTraceErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kAgentTraceErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kAgentTraceErrorCodeCount));
}

TEST(AgentTraceErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (AgentTraceErrorCode c : kAll) {
        std::string s = AgentTraceErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_TRACE_", 0), 0u) << s << " must start with CX_ERR_TRACE_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 7u);
}

// table, row by row.
TEST(AgentTraceErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](AgentTraceErrorCode c, const char* code, ErrorCategory cat, bool retry) {
        const AgentTraceErrorInfo& i = GetAgentTraceErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        // retry_after_ms column is "-" for every agent trace code.
        EXPECT_FALSE(i.retry_after_ms.has_value()) << code;
    };
    chk(AgentTraceErrorCode::kSessionNotFound, "CX_ERR_TRACE_SESSION_NOT_FOUND",
        ErrorCategory::kPermanent, false);
    chk(AgentTraceErrorCode::kInvalidFilter, "CX_ERR_TRACE_INVALID_FILTER",
        ErrorCategory::kPermanent, false);
    chk(AgentTraceErrorCode::kInteractionNotFound, "CX_ERR_TRACE_INTERACTION_NOT_FOUND",
        ErrorCategory::kPermanent, false);
    chk(AgentTraceErrorCode::kUnauthorized, "CX_ERR_TRACE_UNAUTHORIZED",
        ErrorCategory::kAuth, false);
    chk(AgentTraceErrorCode::kMcpSessionInvalid, "CX_ERR_TRACE_MCP_SESSION_INVALID",
        ErrorCategory::kPermanent, false);
    chk(AgentTraceErrorCode::kSessionExpired, "CX_ERR_TRACE_SESSION_EXPIRED",
        ErrorCategory::kPermanent, false);
    chk(AgentTraceErrorCode::kInternal, "CX_ERR_TRACE_INTERNAL",
        ErrorCategory::kTransient, true);
}

TEST(AgentTraceErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kSessionNotFound),
              (std::vector<std::string>{"session_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kInvalidFilter),
              (std::vector<std::string>{"invalid_field", "reason"}));  // value_preview optional
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kInteractionNotFound),
              (std::vector<std::string>{"interaction_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kUnauthorized),
              (std::vector<std::string>{"required_role"}));
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kMcpSessionInvalid),
              (std::vector<std::string>{"session_id", "reason"}));
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kSessionExpired),
              (std::vector<std::string>{"session_id", "retention_days"}));
    EXPECT_EQ(RequiredStructuredDataKeys(AgentTraceErrorCode::kInternal),
              (std::vector<std::string>{"error_id"}));
}

TEST(AgentTraceErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"invalid_field", "X-Session-Id"}, {"reason", "too long"}};
    EXPECT_TRUE(HasRequiredStructuredData(AgentTraceErrorCode::kInvalidFilter, full));
    // value_preview present but not required — still valid.
    full["value_preview"] = "abc";
    EXPECT_TRUE(HasRequiredStructuredData(AgentTraceErrorCode::kInvalidFilter, full));

    nlohmann::json missing = {{"invalid_field", "X-Session-Id"}};
    EXPECT_FALSE(HasRequiredStructuredData(AgentTraceErrorCode::kInvalidFilter, missing));

    // Non-object payload never satisfies a code that requires keys.
    EXPECT_FALSE(HasRequiredStructuredData(AgentTraceErrorCode::kInternal,
                                           nlohmann::json("not-an-object")));
}

TEST(AgentTraceErrorTest, MakeAgentTraceErrorFillsFromRegistry) {
    nlohmann::json sd = {{"session_id", "sess-1"}, {"retention_days", 90}};
    auto err = MakeAgentTraceError(AgentTraceErrorCode::kSessionExpired, sd, "session is past retention");
    EXPECT_EQ(err.code, "CX_ERR_TRACE_SESSION_EXPIRED");
    EXPECT_EQ(err.message, "session is past retention");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["session_id"], "sess-1");
}

TEST(AgentTraceErrorTest, MakeAgentTraceErrorDefaultsMessageToCode) {
    auto err = MakeAgentTraceError(AgentTraceErrorCode::kInternal);
    EXPECT_EQ(err.message, "CX_ERR_TRACE_INTERNAL");
    EXPECT_TRUE(err.retryable);
}

TEST(AgentTraceErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeAgentTraceError(AgentTraceErrorCode::kUnauthorized, {{"required_role", "admin"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_TRACE_UNAUTHORIZED");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "auth");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["required_role"], "admin");
}

TEST(AgentTraceErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = AgentTraceStatus(AgentTraceErrorCode::kSessionNotFound, "no such session");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_TRACE_SESSION_NOT_FOUND"), std::string::npos);
    EXPECT_NE(s.message().find("no such session"), std::string::npos);
}

TEST(AgentTraceErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (AgentTraceErrorCode c : kAll) {
        EXPECT_NE(AgentTraceErrorToStatusCode(c), StatusCode::kOk) << AgentTraceErrorCodeString(c);
    }
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kSessionNotFound), StatusCode::kNotFound);
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kInteractionNotFound), StatusCode::kNotFound);
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kInvalidFilter), StatusCode::kInvalidArgument);
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kMcpSessionInvalid), StatusCode::kInvalidArgument);
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kSessionExpired), StatusCode::kInvalidArgument);
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kUnauthorized), StatusCode::kPermissionDenied);
    EXPECT_EQ(AgentTraceErrorToStatusCode(AgentTraceErrorCode::kInternal), StatusCode::kInternal);
}

}  // namespace
}  // namespace cortrix::agent_trace
