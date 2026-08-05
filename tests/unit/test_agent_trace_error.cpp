#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/agent_trace/agent_trace_error.h"

// S7 coverage: the agent trace error model (template A) — all 7 CX_ERR_F13_* identities,
// their §9.2 attributes (category/retryable/retry_after_ms/structured_data keys),
// the AgentFriendlyError builder, the JSON body, and the Status bridge.
namespace cortrix::agent_trace {
namespace {

using agent_friendly::ErrorCategory;

constexpr F13ErrorCode kAll[] = {
    F13ErrorCode::kSessionNotFound,
    F13ErrorCode::kInvalidFilter,
    F13ErrorCode::kInteractionNotFound,
    F13ErrorCode::kUnauthorized,
    F13ErrorCode::kMcpSessionInvalid,
    F13ErrorCode::kSessionExpired,
    F13ErrorCode::kInternal,
};

TEST(F13ErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kF13ErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kF13ErrorCodeCount));
}

TEST(F13ErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (F13ErrorCode c : kAll) {
        std::string s = F13ErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_F13_", 0), 0u) << s << " must start with CX_ERR_F13_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 7u);
}

// §9.2 table, row by row.
TEST(F13ErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](F13ErrorCode c, const char* code, ErrorCategory cat, bool retry) {
        const F13ErrorInfo& i = GetF13ErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        // §9.2 retry_after_ms column is "-" for every agent trace code.
        EXPECT_FALSE(i.retry_after_ms.has_value()) << code;
    };
    chk(F13ErrorCode::kSessionNotFound, "CX_ERR_F13_SESSION_NOT_FOUND",
        ErrorCategory::kPermanent, false);
    chk(F13ErrorCode::kInvalidFilter, "CX_ERR_F13_INVALID_FILTER",
        ErrorCategory::kPermanent, false);
    chk(F13ErrorCode::kInteractionNotFound, "CX_ERR_F13_INTERACTION_NOT_FOUND",
        ErrorCategory::kPermanent, false);
    chk(F13ErrorCode::kUnauthorized, "CX_ERR_F13_UNAUTHORIZED",
        ErrorCategory::kAuth, false);
    chk(F13ErrorCode::kMcpSessionInvalid, "CX_ERR_F13_MCP_SESSION_INVALID",
        ErrorCategory::kPermanent, false);
    chk(F13ErrorCode::kSessionExpired, "CX_ERR_F13_SESSION_EXPIRED",
        ErrorCategory::kPermanent, false);
    chk(F13ErrorCode::kInternal, "CX_ERR_F13_INTERNAL",
        ErrorCategory::kTransient, true);
}

TEST(F13ErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kSessionNotFound),
              (std::vector<std::string>{"session_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kInvalidFilter),
              (std::vector<std::string>{"invalid_field", "reason"}));  // value_preview optional
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kInteractionNotFound),
              (std::vector<std::string>{"interaction_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kUnauthorized),
              (std::vector<std::string>{"required_role"}));
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kMcpSessionInvalid),
              (std::vector<std::string>{"session_id", "reason"}));
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kSessionExpired),
              (std::vector<std::string>{"session_id", "retention_days"}));
    EXPECT_EQ(RequiredStructuredDataKeys(F13ErrorCode::kInternal),
              (std::vector<std::string>{"error_id"}));
}

TEST(F13ErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"invalid_field", "X-Session-Id"}, {"reason", "too long"}};
    EXPECT_TRUE(HasRequiredStructuredData(F13ErrorCode::kInvalidFilter, full));
    // value_preview present but not required — still valid.
    full["value_preview"] = "abc";
    EXPECT_TRUE(HasRequiredStructuredData(F13ErrorCode::kInvalidFilter, full));

    nlohmann::json missing = {{"invalid_field", "X-Session-Id"}};
    EXPECT_FALSE(HasRequiredStructuredData(F13ErrorCode::kInvalidFilter, missing));

    // Non-object payload never satisfies a code that requires keys.
    EXPECT_FALSE(HasRequiredStructuredData(F13ErrorCode::kInternal,
                                           nlohmann::json("not-an-object")));
}

TEST(F13ErrorTest, MakeF13ErrorFillsFromRegistry) {
    nlohmann::json sd = {{"session_id", "sess-1"}, {"retention_days", 90}};
    auto err = MakeF13Error(F13ErrorCode::kSessionExpired, sd, "session is past retention");
    EXPECT_EQ(err.code, "CX_ERR_F13_SESSION_EXPIRED");
    EXPECT_EQ(err.message, "session is past retention");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["session_id"], "sess-1");
}

TEST(F13ErrorTest, MakeF13ErrorDefaultsMessageToCode) {
    auto err = MakeF13Error(F13ErrorCode::kInternal);
    EXPECT_EQ(err.message, "CX_ERR_F13_INTERNAL");
    EXPECT_TRUE(err.retryable);
}

TEST(F13ErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeF13Error(F13ErrorCode::kUnauthorized, {{"required_role", "admin"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_F13_UNAUTHORIZED");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "auth");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["required_role"], "admin");
}

TEST(F13ErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = F13Status(F13ErrorCode::kSessionNotFound, "no such session");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    EXPECT_NE(s.message().find("CX_ERR_F13_SESSION_NOT_FOUND"), std::string::npos);
    EXPECT_NE(s.message().find("no such session"), std::string::npos);
}

TEST(F13ErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (F13ErrorCode c : kAll) {
        EXPECT_NE(F13ErrorToStatusCode(c), StatusCode::kOk) << F13ErrorCodeString(c);
    }
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kSessionNotFound), StatusCode::kNotFound);
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kInteractionNotFound), StatusCode::kNotFound);
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kInvalidFilter), StatusCode::kInvalidArgument);
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kMcpSessionInvalid), StatusCode::kInvalidArgument);
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kSessionExpired), StatusCode::kInvalidArgument);
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kUnauthorized), StatusCode::kPermissionDenied);
    EXPECT_EQ(F13ErrorToStatusCode(F13ErrorCode::kInternal), StatusCode::kInternal);
}

}  // namespace
}  // namespace cortrix::agent_trace
