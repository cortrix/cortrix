#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/memory/mem03_error.h"

// DEPTH — the CX_ERR_MEMORY_QUOTA (429) contract surface. The DEPTH brief asked
// for the "Create over-quota + memory_routes 429" path; an investigation of the
// frozen tree shows MemoryTransparency::Create and memory_routes.cpp DO NOT
// enforce any per-user/per-NS quota (no quota seam exists on the service or the
// HTTP layer in Phase-1 standalone — the 429 is the registered identity awaiting
// the D3.5 rate-limit wiring). So the over-quota business trigger is NOT
// deterministically reachable; see the REPORT note. What IS reachable + worth
// pinning is the QUOTA error-identity contract a future 429 body must satisfy:
// the structured_data key set (partial vs complete), the Agent-friendly JSON
// projection, and the Status bridge. Suite name is globally unique
// (Mem03QuotaDepth) and complements (does not duplicate) test_mem03_error.cpp.
namespace cortrix::memory::transparency {
namespace {

using agent_friendly::ErrorCategory;

// The §4.3.4.bis QUOTA structured_data contract: a 429 body MUST carry exactly
// these 5 keys so an Agent can read the quota usage/limit/window machine-readably.
TEST(Mem03QuotaDepthTest, QuotaRequiredKeysAreTheFiveSpecKeys) {
    const std::vector<std::string>& keys =
        RequiredStructuredDataKeys(Mem03ErrorCode::kQuota);
    EXPECT_EQ(keys, (std::vector<std::string>{"user_id", "namespace", "quota_used",
                                              "quota_limit", "window_seconds"}));
}

TEST(Mem03QuotaDepthTest, CompleteQuotaStructuredDataPasses) {
    nlohmann::json full = {{"user_id", "u1"}, {"namespace", "ns1"},
                           {"quota_used", 100}, {"quota_limit", 100},
                           {"window_seconds", 60}};
    EXPECT_TRUE(HasRequiredStructuredData(Mem03ErrorCode::kQuota, full));
}

TEST(Mem03QuotaDepthTest, PartialQuotaStructuredDataFailsEachMissingKey) {
    // Drop one required key at a time → each omission must fail the check.
    const std::vector<std::string> keys = {"user_id", "namespace", "quota_used",
                                           "quota_limit", "window_seconds"};
    for (const std::string& drop : keys) {
        nlohmann::json sd = nlohmann::json::object();
        for (const std::string& k : keys) {
            if (k != drop) sd[k] = "x";
        }
        EXPECT_FALSE(HasRequiredStructuredData(Mem03ErrorCode::kQuota, sd))
            << "omitting '" << drop << "' should fail the required-keys check";
    }
}

TEST(Mem03QuotaDepthTest, NonObjectQuotaStructuredDataFails) {
    EXPECT_FALSE(HasRequiredStructuredData(Mem03ErrorCode::kQuota,
                                           nlohmann::json("not-an-object")));
    EXPECT_FALSE(HasRequiredStructuredData(Mem03ErrorCode::kQuota,
                                           nlohmann::json::array()));
}

// The 429 body, fully projected, must surface a machine-readable retry hint
// (GEN-Agent #6) of 60_000 ms + category "quota" + retryable true.
TEST(Mem03QuotaDepthTest, QuotaBodyCarriesRetryHintAndCategory) {
    auto err = MakeMem03Error(
        Mem03ErrorCode::kQuota,
        {{"user_id", "u1"}, {"namespace", "ns1"}, {"quota_used", 100},
         {"quota_limit", 100}, {"window_seconds", 60}},
        "per-user memory quota exceeded");
    EXPECT_EQ(err.code, "CX_ERR_MEMORY_QUOTA");
    EXPECT_EQ(err.message, "per-user memory quota exceeded");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kQuota);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 60000);

    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_MEMORY_QUOTA");
    EXPECT_EQ(j["category"], "quota");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["retry_after_ms"], 60000);
    EXPECT_EQ(j["structured_data"]["quota_limit"], 100);
    EXPECT_EQ(j["structured_data"]["window_seconds"], 60);
}

// The HTTP status accessor must report 429 for QUOTA (the routes layer maps it
// when the rate-limit wiring lands; the registry is the SoT today).
TEST(Mem03QuotaDepthTest, QuotaHttpStatusIs429) {
    EXPECT_EQ(Mem03ErrorHttpStatus(Mem03ErrorCode::kQuota), 429);
    EXPECT_EQ(GetMem03ErrorInfo(Mem03ErrorCode::kQuota).http_status, 429);
}

// The Result<T>/Status bridge folds 429 onto the closest coarse StatusCode
// (kUnavailable) while preserving the exact CX token in the message — so the
// API boundary can re-inflate the 429 + retry hint.
TEST(Mem03QuotaDepthTest, QuotaStatusBridgePreservesTokenUnderUnavailable) {
    Status s = Mem03Status(Mem03ErrorCode::kQuota, "per-ns rate limit");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_EQ(Mem03ErrorToStatusCode(Mem03ErrorCode::kQuota), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("CX_ERR_MEMORY_QUOTA"), std::string::npos);
    EXPECT_NE(s.message().find("per-ns rate limit"), std::string::npos);
}

TEST(Mem03QuotaDepthTest, QuotaStatusBridgeDefaultsMessageToTokenOnly) {
    Status s = Mem03Status(Mem03ErrorCode::kQuota);
    EXPECT_EQ(s.message(), "CX_ERR_MEMORY_QUOTA");
}

}  // namespace
}  // namespace cortrix::memory::transparency
