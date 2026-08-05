#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/memory/memory_opt_out_error.h"

// S5 coverage: the memory opt-out error model (template A) — all 7 CX_ERR_MEMOPTOUT_* identities,
// their ARCH §4.1.11 attributes (http/category/retryable/retry_after_ms/structured_data
// keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::memory::immunity {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kMemoryOptOutErrorCodeCount).
constexpr MemoryOptOutErrorCode kAll[] = {
    MemoryOptOutErrorCode::kSessionNotFound,
    MemoryOptOutErrorCode::kAlreadyOptedOut,
    MemoryOptOutErrorCode::kNotOptedOut,
    MemoryOptOutErrorCode::kRevokeDenied,
    MemoryOptOutErrorCode::kOptOutDisabled,
    MemoryOptOutErrorCode::kInvalidSessionId,
    MemoryOptOutErrorCode::kMetadataTooLarge,
};

TEST(MemoryOptOutErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMemoryOptOutErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMemoryOptOutErrorCodeCount));
}

TEST(MemoryOptOutErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (MemoryOptOutErrorCode c : kAll) {
        std::string s = MemoryOptOutErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_MEMOPTOUT_", 0), 0u) << s << " must start with CX_ERR_MEMOPTOUT_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 7u);
}

// ARCH §4.1.11 memory opt-out table, row by row.
TEST(MemoryOptOutErrorTest, RegistryMatchesArchTable) {
    auto chk = [](MemoryOptOutErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const MemoryOptOutErrorInfo& i = GetMemoryOptOutErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(MemoryOptOutErrorCode::kSessionNotFound, "CX_ERR_MEMOPTOUT_SESSION_NOT_FOUND", 404,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryOptOutErrorCode::kAlreadyOptedOut, "CX_ERR_MEMOPTOUT_ALREADY_OPTED_OUT", 409,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryOptOutErrorCode::kNotOptedOut, "CX_ERR_MEMOPTOUT_NOT_OPTED_OUT", 409,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryOptOutErrorCode::kRevokeDenied, "CX_ERR_MEMOPTOUT_REVOKE_DENIED", 403,
        ErrorCategory::kAuth, false, std::nullopt);
    chk(MemoryOptOutErrorCode::kOptOutDisabled, "CX_ERR_MEMOPTOUT_DISABLED", 503,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryOptOutErrorCode::kInvalidSessionId, "CX_ERR_MEMOPTOUT_INVALID_SESSION_ID", 422,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(MemoryOptOutErrorCode::kMetadataTooLarge, "CX_ERR_MEMOPTOUT_METADATA_TOO_LARGE", 422,
        ErrorCategory::kPermanent, false, std::nullopt);
}

TEST(MemoryOptOutErrorTest, AllCodesNonRetryableWithoutHint) {
    // GEN-Agent #6: all 7 memory opt-out faults are permanent/auth client errors → none is
    // retryable and none carries a retry hint.
    for (MemoryOptOutErrorCode c : kAll) {
        const MemoryOptOutErrorInfo& i = GetMemoryOptOutErrorInfo(c);
        EXPECT_FALSE(i.retryable) << i.cx_code;
        EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
    }
}

TEST(MemoryOptOutErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kSessionNotFound), 404);
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kAlreadyOptedOut), 409);
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kNotOptedOut), 409);
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kRevokeDenied), 403);
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kOptOutDisabled), 503);
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kInvalidSessionId), 422);
    EXPECT_EQ(MemoryOptOutErrorHttpStatus(MemoryOptOutErrorCode::kMetadataTooLarge), 422);
}

TEST(MemoryOptOutErrorTest, RequiredStructuredDataKeysPerArch) {
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kSessionNotFound),
              (std::vector<std::string>{"session_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kAlreadyOptedOut),
              (std::vector<std::string>{"session_id", "opted_out_at"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kNotOptedOut),
              (std::vector<std::string>{"session_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kRevokeDenied),
              (std::vector<std::string>{"required_role"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kOptOutDisabled),
              (std::vector<std::string>{"config_source"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kInvalidSessionId),
              (std::vector<std::string>{"session_id", "expected_format"}));
    EXPECT_EQ(RequiredStructuredDataKeys(MemoryOptOutErrorCode::kMetadataTooLarge),
              (std::vector<std::string>{"metadata_bytes", "max_bytes"}));
}

TEST(MemoryOptOutErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"session_id", "s_abc"}, {"opted_out_at", "2026-05-16T10:30:00Z"}};
    EXPECT_TRUE(HasRequiredStructuredData(MemoryOptOutErrorCode::kAlreadyOptedOut, full));

    nlohmann::json missing = {{"session_id", "s_abc"}};
    EXPECT_FALSE(HasRequiredStructuredData(MemoryOptOutErrorCode::kAlreadyOptedOut, missing));

    // Non-object payload only passes when no keys are required (all 7 require keys).
    EXPECT_FALSE(HasRequiredStructuredData(MemoryOptOutErrorCode::kSessionNotFound,
                                           nlohmann::json("not-an-object")));
}

TEST(MemoryOptOutErrorTest, MakeMemoryOptOutErrorFillsFromRegistry) {
    nlohmann::json sd = {{"session_id", "s_123"}, {"opted_out_at", "2026-05-16T10:30:00Z"}};
    auto err = MakeMemoryOptOutError(MemoryOptOutErrorCode::kAlreadyOptedOut, sd,
                              "session already opted out");
    EXPECT_EQ(err.code, "CX_ERR_MEMOPTOUT_ALREADY_OPTED_OUT");
    EXPECT_EQ(err.message, "session already opted out");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["session_id"], "s_123");
}

TEST(MemoryOptOutErrorTest, MakeMemoryOptOutErrorDefaultsMessageToCode) {
    auto err = MakeMemoryOptOutError(MemoryOptOutErrorCode::kSessionNotFound);
    EXPECT_EQ(err.message, "CX_ERR_MEMOPTOUT_SESSION_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(MemoryOptOutErrorTest, RevokeDeniedIsAuthCategory) {
    auto err = MakeMemoryOptOutError(MemoryOptOutErrorCode::kRevokeDenied, {{"required_role", "admin"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEMOPTOUT_REVOKE_DENIED");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "auth");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["required_role"], "admin");
}

TEST(MemoryOptOutErrorTest, ToJsonSerializesNotFoundBody) {
    auto err = MakeMemoryOptOutError(MemoryOptOutErrorCode::kSessionNotFound, {{"session_id", "s_002"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEMOPTOUT_SESSION_NOT_FOUND");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["session_id"], "s_002");
}

TEST(MemoryOptOutErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = MemoryOptOutStatus(MemoryOptOutErrorCode::kRevokeDenied, "caller is not admin");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_MEMOPTOUT_REVOKE_DENIED"), std::string::npos);
    EXPECT_NE(s.message().find("caller is not admin"), std::string::npos);
}

TEST(MemoryOptOutErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (MemoryOptOutErrorCode c : kAll) {
        StatusCode sc = MemoryOptOutErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << MemoryOptOutErrorCodeString(c);
    }
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kSessionNotFound),
              StatusCode::kNotFound);
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kAlreadyOptedOut),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kNotOptedOut),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kRevokeDenied),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kOptOutDisabled),
              StatusCode::kUnavailable);
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kInvalidSessionId),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(MemoryOptOutErrorToStatusCode(MemoryOptOutErrorCode::kMetadataTooLarge),
              StatusCode::kInvalidArgument);
}

TEST(MemoryOptOutErrorTest, StatusBridgeDefaultsMessageToCodeOnly) {
    Status s = MemoryOptOutStatus(MemoryOptOutErrorCode::kNotOptedOut);
    EXPECT_EQ(s.message(), "CX_ERR_MEMOPTOUT_NOT_OPTED_OUT");
}

}  // namespace
}  // namespace cortrix::memory::immunity
