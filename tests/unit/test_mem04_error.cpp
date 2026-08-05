#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/memory/mem04_error.h"

// S5 coverage: the memory opt-out error model (template A) — all 7 CX_ERR_MEM04_* identities,
// their ARCH §4.1.11 attributes (http/category/retryable/retry_after_ms/structured_data
// keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::memory::immunity {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kMem04ErrorCodeCount).
constexpr Mem04ErrorCode kAll[] = {
    Mem04ErrorCode::kSessionNotFound,
    Mem04ErrorCode::kAlreadyOptedOut,
    Mem04ErrorCode::kNotOptedOut,
    Mem04ErrorCode::kRevokeDenied,
    Mem04ErrorCode::kOptOutDisabled,
    Mem04ErrorCode::kInvalidSessionId,
    Mem04ErrorCode::kMetadataTooLarge,
};

TEST(Mem04ErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kMem04ErrorCodeCount, 7);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kMem04ErrorCodeCount));
}

TEST(Mem04ErrorTest, AllCodesHaveUniqueCxStrings) {
    std::set<std::string> seen;
    for (Mem04ErrorCode c : kAll) {
        std::string s = Mem04ErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_MEM04_", 0), 0u) << s << " must start with CX_ERR_MEM04_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 7u);
}

// ARCH §4.1.11 memory opt-out table, row by row.
TEST(Mem04ErrorTest, RegistryMatchesArchTable) {
    auto chk = [](Mem04ErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const Mem04ErrorInfo& i = GetMem04ErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(Mem04ErrorCode::kSessionNotFound, "CX_ERR_MEM04_SESSION_NOT_FOUND", 404,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem04ErrorCode::kAlreadyOptedOut, "CX_ERR_MEM04_ALREADY_OPTED_OUT", 409,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem04ErrorCode::kNotOptedOut, "CX_ERR_MEM04_NOT_OPTED_OUT", 409,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem04ErrorCode::kRevokeDenied, "CX_ERR_MEM04_REVOKE_DENIED", 403,
        ErrorCategory::kAuth, false, std::nullopt);
    chk(Mem04ErrorCode::kOptOutDisabled, "CX_ERR_MEM04_OPT_OUT_DISABLED", 503,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem04ErrorCode::kInvalidSessionId, "CX_ERR_MEM04_INVALID_SESSION_ID", 422,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(Mem04ErrorCode::kMetadataTooLarge, "CX_ERR_MEM04_METADATA_TOO_LARGE", 422,
        ErrorCategory::kPermanent, false, std::nullopt);
}

TEST(Mem04ErrorTest, AllCodesNonRetryableWithoutHint) {
    // GEN-Agent #6: all 7 memory opt-out faults are permanent/auth client errors → none is
    // retryable and none carries a retry hint.
    for (Mem04ErrorCode c : kAll) {
        const Mem04ErrorInfo& i = GetMem04ErrorInfo(c);
        EXPECT_FALSE(i.retryable) << i.cx_code;
        EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
    }
}

TEST(Mem04ErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kSessionNotFound), 404);
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kAlreadyOptedOut), 409);
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kNotOptedOut), 409);
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kRevokeDenied), 403);
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kOptOutDisabled), 503);
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kInvalidSessionId), 422);
    EXPECT_EQ(Mem04ErrorHttpStatus(Mem04ErrorCode::kMetadataTooLarge), 422);
}

TEST(Mem04ErrorTest, RequiredStructuredDataKeysPerArch) {
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kSessionNotFound),
              (std::vector<std::string>{"session_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kAlreadyOptedOut),
              (std::vector<std::string>{"session_id", "opted_out_at"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kNotOptedOut),
              (std::vector<std::string>{"session_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kRevokeDenied),
              (std::vector<std::string>{"required_role"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kOptOutDisabled),
              (std::vector<std::string>{"config_source"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kInvalidSessionId),
              (std::vector<std::string>{"session_id", "expected_format"}));
    EXPECT_EQ(RequiredStructuredDataKeys(Mem04ErrorCode::kMetadataTooLarge),
              (std::vector<std::string>{"metadata_bytes", "max_bytes"}));
}

TEST(Mem04ErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"session_id", "s_abc"}, {"opted_out_at", "2026-05-16T10:30:00Z"}};
    EXPECT_TRUE(HasRequiredStructuredData(Mem04ErrorCode::kAlreadyOptedOut, full));

    nlohmann::json missing = {{"session_id", "s_abc"}};
    EXPECT_FALSE(HasRequiredStructuredData(Mem04ErrorCode::kAlreadyOptedOut, missing));

    // Non-object payload only passes when no keys are required (all 7 require keys).
    EXPECT_FALSE(HasRequiredStructuredData(Mem04ErrorCode::kSessionNotFound,
                                           nlohmann::json("not-an-object")));
}

TEST(Mem04ErrorTest, MakeMem04ErrorFillsFromRegistry) {
    nlohmann::json sd = {{"session_id", "s_123"}, {"opted_out_at", "2026-05-16T10:30:00Z"}};
    auto err = MakeMem04Error(Mem04ErrorCode::kAlreadyOptedOut, sd,
                              "session already opted out");
    EXPECT_EQ(err.code, "CX_ERR_MEM04_ALREADY_OPTED_OUT");
    EXPECT_EQ(err.message, "session already opted out");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["session_id"], "s_123");
}

TEST(Mem04ErrorTest, MakeMem04ErrorDefaultsMessageToCode) {
    auto err = MakeMem04Error(Mem04ErrorCode::kSessionNotFound);
    EXPECT_EQ(err.message, "CX_ERR_MEM04_SESSION_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(Mem04ErrorTest, RevokeDeniedIsAuthCategory) {
    auto err = MakeMem04Error(Mem04ErrorCode::kRevokeDenied, {{"required_role", "admin"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEM04_REVOKE_DENIED");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "auth");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["required_role"], "admin");
}

TEST(Mem04ErrorTest, ToJsonSerializesNotFoundBody) {
    auto err = MakeMem04Error(Mem04ErrorCode::kSessionNotFound, {{"session_id", "s_002"}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEM04_SESSION_NOT_FOUND");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    EXPECT_EQ(j["structured_data"]["session_id"], "s_002");
}

TEST(Mem04ErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = Mem04Status(Mem04ErrorCode::kRevokeDenied, "caller is not admin");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_MEM04_REVOKE_DENIED"), std::string::npos);
    EXPECT_NE(s.message().find("caller is not admin"), std::string::npos);
}

TEST(Mem04ErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (Mem04ErrorCode c : kAll) {
        StatusCode sc = Mem04ErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << Mem04ErrorCodeString(c);
    }
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kSessionNotFound),
              StatusCode::kNotFound);
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kAlreadyOptedOut),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kNotOptedOut),
              StatusCode::kAlreadyExists);
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kRevokeDenied),
              StatusCode::kPermissionDenied);
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kOptOutDisabled),
              StatusCode::kUnavailable);
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kInvalidSessionId),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(Mem04ErrorToStatusCode(Mem04ErrorCode::kMetadataTooLarge),
              StatusCode::kInvalidArgument);
}

TEST(Mem04ErrorTest, StatusBridgeDefaultsMessageToCodeOnly) {
    Status s = Mem04Status(Mem04ErrorCode::kNotOptedOut);
    EXPECT_EQ(s.message(), "CX_ERR_MEM04_NOT_OPTED_OUT");
}

}  // namespace
}  // namespace cortrix::memory::immunity
