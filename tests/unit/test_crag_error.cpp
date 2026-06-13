#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/retrieval/crag_error.h"

// F37 S1 coverage: the CRAG error model (template A) — all 4 CX_ERR_F37_*
// identities, their §4.3 attributes (category / retryable / retry_after_ms /
// structured_data keys), the AgentFriendlyError builder, and the Status bridge.
namespace cortrix::retrieval {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kCragErrorCodeCount).
constexpr CragErrorCode kAll[] = {
    CragErrorCode::kClassifierLoadFailed,
    CragErrorCode::kInferenceFailed,
    CragErrorCode::kThresholdInvalid,
    CragErrorCode::kFallbackTriggered,
};

TEST(CragErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kCragErrorCodeCount, 4);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kCragErrorCodeCount));
}

TEST(CragErrorTest, AllCodesHaveUniqueF37CxStrings) {
    std::set<std::string> seen;
    for (CragErrorCode c : kAll) {
        std::string s = CragErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_F37_", 0), 0u) << s << " must start with CX_ERR_F37_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 4u);
}

// §4.3 table, row by row.
TEST(CragErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](CragErrorCode c, const char* code, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const CragErrorInfo& i = GetCragErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(CragErrorCode::kClassifierLoadFailed, "CX_ERR_F37_CLASSIFIER_LOAD_FAILED",
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(CragErrorCode::kInferenceFailed, "CX_ERR_F37_INFERENCE_FAILED",
        ErrorCategory::kTransient, true, 200);
    chk(CragErrorCode::kThresholdInvalid, "CX_ERR_F37_THRESHOLD_INVALID",
        ErrorCategory::kPermanent, false, std::nullopt);
    // §4.3 lists FALLBACK_TRIGGERED as transient + retryable with retry_after_ms
    // "-" (unspecified) → null. It is informational (the transparent all-Correct
    // degrade already happened), so no concrete back-off is advertised.
    chk(CragErrorCode::kFallbackTriggered, "CX_ERR_F37_FALLBACK_TRIGGERED",
        ErrorCategory::kTransient, true, std::nullopt);
}

TEST(CragErrorTest, NonRetryablesCarryNoRetryHint) {
    // The two permanent codes carry no retry hint (GEN-Agent #6).
    EXPECT_FALSE(GetCragErrorInfo(CragErrorCode::kClassifierLoadFailed).retry_after_ms.has_value());
    EXPECT_FALSE(GetCragErrorInfo(CragErrorCode::kThresholdInvalid).retry_after_ms.has_value());
    // INFERENCE_FAILED is retryable AND advertises a positive back-off.
    const auto& inf = GetCragErrorInfo(CragErrorCode::kInferenceFailed);
    ASSERT_TRUE(inf.retry_after_ms.has_value());
    EXPECT_GT(*inf.retry_after_ms, 0);
}

TEST(CragErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(CragErrorCode::kClassifierLoadFailed),
              (std::vector<std::string>{"model_path", "version"}));
    EXPECT_EQ(RequiredStructuredDataKeys(CragErrorCode::kInferenceFailed),
              (std::vector<std::string>{"chunk_id", "retry_count"}));
    EXPECT_EQ(RequiredStructuredDataKeys(CragErrorCode::kThresholdInvalid),
              (std::vector<std::string>{"invalid_field", "value"}));
    EXPECT_EQ(RequiredStructuredDataKeys(CragErrorCode::kFallbackTriggered),
              (std::vector<std::string>{"fallback_reason"}));
}

TEST(CragErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"chunk_id", 12345}, {"retry_count", 3}};
    EXPECT_TRUE(HasRequiredStructuredData(CragErrorCode::kInferenceFailed, full));

    nlohmann::json missing = {{"chunk_id", 12345}};
    EXPECT_FALSE(HasRequiredStructuredData(CragErrorCode::kInferenceFailed, missing));

    // Non-object payload never satisfies a code that requires keys.
    EXPECT_FALSE(HasRequiredStructuredData(CragErrorCode::kFallbackTriggered,
                                           nlohmann::json("not-an-object")));
}

TEST(CragErrorTest, EveryCodeRequiresAtLeastOneStructuredKey) {
    // All 4 F37 codes have a non-empty structured_data contract (§4.3).
    for (CragErrorCode c : kAll) {
        EXPECT_FALSE(RequiredStructuredDataKeys(c).empty()) << CragErrorCodeString(c);
    }
}

TEST(CragErrorTest, MakeCragErrorFillsFromRegistry) {
    nlohmann::json sd = {{"chunk_id", 7}, {"retry_count", 3}};
    auto err = MakeCragError(CragErrorCode::kInferenceFailed, sd,
                             "CRAG classifier inference failed, falling back to correct");
    EXPECT_EQ(err.code, "CX_ERR_F37_INFERENCE_FAILED");
    EXPECT_EQ(err.message, "CRAG classifier inference failed, falling back to correct");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 200);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["chunk_id"], 7);
}

TEST(CragErrorTest, MakeCragErrorDefaultsMessageToCode) {
    auto err = MakeCragError(CragErrorCode::kClassifierLoadFailed);
    EXPECT_EQ(err.message, "CX_ERR_F37_CLASSIFIER_LOAD_FAILED");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(CragErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeCragError(CragErrorCode::kInferenceFailed,
                             {{"chunk_id", 12345}, {"retry_count", 3}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_F37_INFERENCE_FAILED");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "transient");
    EXPECT_EQ(j["retry_after_ms"], 200);
    EXPECT_EQ(j["structured_data"]["retry_count"], 3);
}

TEST(CragErrorTest, StatusBridgeCarriesCodeToken) {
    Status s = CragStatus(CragErrorCode::kThresholdInvalid, "threshold_correct=1.5");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_F37_THRESHOLD_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("threshold_correct=1.5"), std::string::npos);
}

TEST(CragErrorTest, StatusCodeMappingIsTotalAndSane) {
    for (CragErrorCode c : kAll) {
        StatusCode sc = CragErrorToStatusCode(c);
        EXPECT_NE(sc, StatusCode::kOk) << CragErrorCodeString(c);
    }
    // Permanent/operator-side → kInvalidArgument; transient → kUnavailable.
    EXPECT_EQ(CragErrorToStatusCode(CragErrorCode::kClassifierLoadFailed),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CragErrorToStatusCode(CragErrorCode::kThresholdInvalid),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(CragErrorToStatusCode(CragErrorCode::kInferenceFailed),
              StatusCode::kUnavailable);
    EXPECT_EQ(CragErrorToStatusCode(CragErrorCode::kFallbackTriggered),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::retrieval
