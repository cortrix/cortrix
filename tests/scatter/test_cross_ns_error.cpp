#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/query/cross_ns_error.h"

// S2.2 coverage: the 6 cross-NS error codes — CX_ERR_ identity,
// category mapping, retryability, structured_data contract, and the GEN-Agent
// 4-field boundary factory. cross-NS query's error responses are the project-level
// Agent-friendly reference, so all 7 principles are asserted here.
namespace cortrix::query {
namespace {

using agent_friendly::ErrorCategory;

// All 6 codes, in enum order. Explicit (not a loop over ints) so the test itself
// documents the locked set and fails to compile if an enumerator is gone.
const std::vector<CrossNsErrorCode>& AllCodes() {
    static const std::vector<CrossNsErrorCode> codes = {
        CrossNsErrorCode::kAuthInvalidCredentials,
        CrossNsErrorCode::kNsUnauthorized,
        CrossNsErrorCode::kTooManyNamespaces,
        CrossNsErrorCode::kNsTimeout,
        CrossNsErrorCode::kIndexCorrupt,
        CrossNsErrorCode::kScatterTimeout,
    };
    return codes;
}

TEST(CrossNsErrorTest, SixCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 6u);
    EXPECT_EQ(kCrossNsErrorCodeCount, 6);
}

// Every code's CX_ERR_* string is unique and matches the API spec ErrorResponseV1
// pattern ^CX_ERR_[A-Z][A-Z_]*$ (GEN-Agent #1 + #7 stable identity).
TEST(CrossNsErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (CrossNsErrorCode code : AllCodes()) {
        std::string cx = CrossNsErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << "bad code string: " << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate code string: " << cx;
    }
    EXPECT_EQ(seen.size(), 6u);
}

// The exact 6 CX_ERR_* strings — locks the contract values.
TEST(CrossNsErrorTest, ExactCodeStrings) {
    EXPECT_STREQ(CrossNsErrorCodeString(CrossNsErrorCode::kAuthInvalidCredentials),
                 "CX_ERR_AUTH_INVALID_CREDENTIALS");
    EXPECT_STREQ(CrossNsErrorCodeString(CrossNsErrorCode::kNsUnauthorized),
                 "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_STREQ(CrossNsErrorCodeString(CrossNsErrorCode::kTooManyNamespaces),
                 "CX_ERR_TOO_MANY_NAMESPACES");
    EXPECT_STREQ(CrossNsErrorCodeString(CrossNsErrorCode::kNsTimeout), "CX_ERR_NS_TIMEOUT");
    EXPECT_STREQ(CrossNsErrorCodeString(CrossNsErrorCode::kIndexCorrupt), "CX_ERR_INDEX_CORRUPT");
    EXPECT_STREQ(CrossNsErrorCodeString(CrossNsErrorCode::kScatterTimeout),
                 "CX_ERR_SCATTER_TIMEOUT");
}

// decision matrix: category + retryability + http status per code.
TEST(CrossNsErrorTest, CategoryRetryabilityHttpMatrix) {
    struct Row {
        CrossNsErrorCode code;
        ErrorCategory category;
        bool retryable;
        int http;
    };
    const std::vector<Row> rows = {
        {CrossNsErrorCode::kAuthInvalidCredentials, ErrorCategory::kAuth,      false, 401},
        {CrossNsErrorCode::kNsUnauthorized,         ErrorCategory::kAuth,      false, 403},
        {CrossNsErrorCode::kTooManyNamespaces,      ErrorCategory::kQuota,     false, 400},
        {CrossNsErrorCode::kNsTimeout,              ErrorCategory::kTimeout,   true,  200},
        {CrossNsErrorCode::kIndexCorrupt,           ErrorCategory::kPermanent, false, 200},
        {CrossNsErrorCode::kScatterTimeout,         ErrorCategory::kTimeout,   true,  200},
    };
    for (const auto& r : rows) {
        const CrossNsErrorInfo& info = GetCrossNsErrorInfo(r.code);
        EXPECT_EQ(info.category, r.category) << CrossNsErrorCodeString(r.code);
        EXPECT_EQ(info.retryable, r.retryable) << CrossNsErrorCodeString(r.code);
        EXPECT_EQ(info.http_status, r.http) << CrossNsErrorCodeString(r.code);
    }
}

// GEN-Agent #6: retry_after_ms is present iff retryable; the two timeout codes use
// 1000ms, everything else is null.
TEST(CrossNsErrorTest, RetryAfterMsConsistentWithRetryable) {
    for (CrossNsErrorCode code : AllCodes()) {
        const CrossNsErrorInfo& info = GetCrossNsErrorInfo(code);
        if (info.retryable) {
            ASSERT_TRUE(info.retry_after_ms.has_value()) << CrossNsErrorCodeString(code);
            EXPECT_GT(*info.retry_after_ms, 0);
        } else {
            EXPECT_FALSE(info.retry_after_ms.has_value()) << CrossNsErrorCodeString(code);
        }
    }
    EXPECT_EQ(GetCrossNsErrorInfo(CrossNsErrorCode::kNsTimeout).retry_after_ms.value_or(0), 1000);
    EXPECT_EQ(GetCrossNsErrorInfo(CrossNsErrorCode::kScatterTimeout).retry_after_ms.value_or(0),
              1000);
}

// MakeCrossNsError fills category/retryable/retry_after_ms from the registry and
// attaches structured_data — the GEN-Agent 4 field boundary.
TEST(CrossNsErrorTest, MakeCrossNsErrorFillsAgentFriendlyFields) {
    auto err = MakeCrossNsError(CrossNsErrorCode::kNsTimeout,
                                nlohmann::json{{"namespace", "ns_b"}}, "ns_b timed out");
    EXPECT_EQ(err.code, "CX_ERR_NS_TIMEOUT");
    EXPECT_EQ(err.message, "ns_b timed out");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTimeout);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 1000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["namespace"], "ns_b");
}

// Empty message defaults to the code string (never an empty human message).
TEST(CrossNsErrorTest, MakeCrossNsErrorDefaultMessageIsCode) {
    auto err = MakeCrossNsError(CrossNsErrorCode::kIndexCorrupt);
    EXPECT_EQ(err.message, "CX_ERR_INDEX_CORRUPT");
}

// MakeNsUnauthorizedError packs unauthorized_namespaces (GEN-Agent #5 — Agent
// extracts it to re-query authorized NS only).
TEST(CrossNsErrorTest, NsUnauthorizedCarriesStructuredNamespaceList) {
    auto err = MakeNsUnauthorizedError({"finance", "hr"});
    EXPECT_EQ(err.code, "CX_ERR_NS_UNAUTHORIZED");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kAuth);
    ASSERT_TRUE(err.structured_data.has_value());
    auto list = (*err.structured_data)["unauthorized_namespaces"];
    ASSERT_TRUE(list.is_array());
    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0], "finance");
    EXPECT_EQ(list[1], "hr");
    // HasRequiredStructuredData agrees the body is complete.
    EXPECT_TRUE(HasRequiredStructuredData(CrossNsErrorCode::kNsUnauthorized, *err.structured_data));
}

// Required structured_data keys per code (GEN-Agent #5 contract SoT).
TEST(CrossNsErrorTest, RequiredStructuredDataKeys) {
    EXPECT_TRUE(RequiredStructuredDataKeys(CrossNsErrorCode::kAuthInvalidCredentials).empty());
    EXPECT_EQ(RequiredStructuredDataKeys(CrossNsErrorCode::kNsUnauthorized),
              (std::vector<std::string>{"unauthorized_namespaces"}));
    EXPECT_EQ(RequiredStructuredDataKeys(CrossNsErrorCode::kTooManyNamespaces),
              (std::vector<std::string>{"requested_count", "max_namespaces"}));
    EXPECT_EQ(RequiredStructuredDataKeys(CrossNsErrorCode::kNsTimeout),
              (std::vector<std::string>{"namespace"}));
    EXPECT_EQ(RequiredStructuredDataKeys(CrossNsErrorCode::kIndexCorrupt),
              (std::vector<std::string>{"namespace", "index_state"}));
}

// HasRequiredStructuredData rejects a body missing a required key.
TEST(CrossNsErrorTest, HasRequiredStructuredDataDetectsMissingKey) {
    EXPECT_FALSE(HasRequiredStructuredData(CrossNsErrorCode::kTooManyNamespaces,
                                           nlohmann::json{{"requested_count", 150}}));
    EXPECT_TRUE(HasRequiredStructuredData(
        CrossNsErrorCode::kTooManyNamespaces,
        nlohmann::json{{"requested_count", 150}, {"max_namespaces", 100}}));
}

// The serialized error body has every GEN-Agent #1/#4/#5/#6 field (the Agent-friendly contract
//) and a valid category enum string.
TEST(CrossNsErrorTest, SerializedBodyIsAgentFriendlySchemaCompliant) {
    for (CrossNsErrorCode code : AllCodes()) {
        auto err = MakeCrossNsError(code, nlohmann::json::object(), "");
        auto j = agent_friendly::ToJson(err);
        ASSERT_TRUE(j.contains("code"));
        ASSERT_TRUE(j.contains("message"));
        ASSERT_TRUE(j.contains("retryable"));
        ASSERT_TRUE(j.contains("category"));
        ASSERT_TRUE(j.contains("retry_after_ms"));
        ASSERT_TRUE(j.contains("structured_data"));
        const std::string cat = j["category"].get<std::string>();
        EXPECT_TRUE(cat == "auth" || cat == "quota" || cat == "transient" ||
                    cat == "permanent" || cat == "timeout")
            << "bad category: " << cat;
    }
}

// CrossNsException carries the full Agent-friendly error + the originating code.
TEST(CrossNsErrorTest, ExceptionCarriesCodeAndError) {
    try {
        throw CrossNsException(CrossNsErrorCode::kTooManyNamespaces,
                               nlohmann::json{{"requested_count", 200}, {"max_namespaces", 100}});
    } catch (const CrossNsException& e) {
        EXPECT_EQ(e.code(), CrossNsErrorCode::kTooManyNamespaces);
        EXPECT_EQ(e.GetError().code, "CX_ERR_TOO_MANY_NAMESPACES");
        EXPECT_EQ(e.GetError().category, ErrorCategory::kQuota);
        ASSERT_TRUE(e.GetError().structured_data.has_value());
        EXPECT_EQ((*e.GetError().structured_data)["requested_count"], 200);
    }
}

// CrossNsStatus bridges to a plain Status with the CX_ERR_ token recoverable in
// the message (F-FREEZE-1 Result<T>/Status surface).
TEST(CrossNsErrorTest, CrossNsStatusEmbedsCodeToken) {
    Status s = CrossNsStatus(CrossNsErrorCode::kNsUnauthorized, "ns_b");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kPermissionDenied);
    EXPECT_NE(s.message().find("CX_ERR_NS_UNAUTHORIZED"), std::string::npos);
    EXPECT_NE(s.message().find("ns_b"), std::string::npos);
}

}  // namespace
}  // namespace cortrix::query
