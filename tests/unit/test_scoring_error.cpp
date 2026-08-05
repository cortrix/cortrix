#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/scoring/scoring_error.h"

// Semantic score S8 / §4.4 coverage: the 2 semantic score error identities — CX_ERR_SCORING_* identity, category
// mapping, retryability, the GEN-Agent 4-field boundary factory, the structured_data
// contract, and the Status bridge. Mirrors tests/unit/test_import_error.cpp (template A).
namespace cortrix::scoring {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<ScoringErrorCode>& AllCodes() {
    static const std::vector<ScoringErrorCode> codes = {
        ScoringErrorCode::kLevelInvalid,
        ScoringErrorCode::kConfigInvalid,
    };
    return codes;
}

TEST(ScoringErrorTest, TwoCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 2u);
    EXPECT_EQ(kScoringErrorCodeCount, 2);
}

TEST(ScoringErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_SCORING_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (ScoringErrorCode code : AllCodes()) {
        std::string cx = ScoringErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate " << cx;
    }
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(std::string(ScoringErrorCodeString(ScoringErrorCode::kLevelInvalid)),
              "CX_ERR_SCORING_LEVEL_INVALID");
    EXPECT_EQ(std::string(ScoringErrorCodeString(ScoringErrorCode::kConfigInvalid)),
              "CX_ERR_SCORING_CONFIG_INVALID");
}

// §4.4: both permanent, neither retryable, retry_after_ms null.
TEST(ScoringErrorTest, RegistryMatchesSpecTable) {
    for (ScoringErrorCode code : AllCodes()) {
        const ScoringErrorInfo& info = GetScoringErrorInfo(code);
        EXPECT_EQ(info.category, ErrorCategory::kPermanent) << ScoringErrorCodeString(code);
        EXPECT_FALSE(info.retryable) << ScoringErrorCodeString(code);
        EXPECT_FALSE(info.retry_after_ms.has_value()) << ScoringErrorCodeString(code);
    }
}

// MakeScoringError fills the 4 GEN-Agent fields from the registry + serializes to the
// §3.1 error body.
TEST(ScoringErrorTest, MakeErrorPopulatesAgentFriendlyFields) {
    auto err = MakeScoringError(
        ScoringErrorCode::kConfigInvalid,
        {{"alpha_received", 1.5}, {"valid_range", {0.0, 1.0}},
         {"config_source", "config.yaml:scoring.alpha"}},
        "alpha out of [0,1]");
    EXPECT_EQ(err.code, "CX_ERR_SCORING_CONFIG_INVALID");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["config_source"], "config.yaml:scoring.alpha");

    auto body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_SCORING_CONFIG_INVALID");
    EXPECT_EQ(body["category"], "permanent");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
}

TEST(ScoringErrorTest, EmptyMessageFallsBackToCode) {
    auto err = MakeScoringError(ScoringErrorCode::kLevelInvalid);
    EXPECT_EQ(err.message, "CX_ERR_SCORING_LEVEL_INVALID");
}

// §4.4 structured_data contract (GEN-Agent #5).
TEST(ScoringErrorTest, RequiredStructuredDataContract) {
    nlohmann::json level_full = {
        {"level_received", 7}, {"max_allowed", 4}, {"scoring_input", nullptr}};
    EXPECT_TRUE(HasRequiredStructuredData(ScoringErrorCode::kLevelInvalid, level_full));
    EXPECT_FALSE(HasRequiredStructuredData(ScoringErrorCode::kLevelInvalid,
                                           nlohmann::json{{"level_received", 7}}));

    nlohmann::json cfg_full = {
        {"alpha_received", 1.5}, {"valid_range", {0.0, 1.0}}, {"config_source", "x"}};
    EXPECT_TRUE(HasRequiredStructuredData(ScoringErrorCode::kConfigInvalid, cfg_full));
    EXPECT_FALSE(HasRequiredStructuredData(ScoringErrorCode::kConfigInvalid, nlohmann::json("x")));
}

// ScoringStatus bridges to a coarse Status whose message recovers the exact identity.
TEST(ScoringErrorTest, StatusBridgePreservesIdentityInMessage) {
    Status s = ScoringStatus(ScoringErrorCode::kLevelInvalid, "level 7");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_SCORING_LEVEL_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("level 7"), std::string::npos);

    EXPECT_EQ(ScoringStatus(ScoringErrorCode::kConfigInvalid).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(ScoringStatus(ScoringErrorCode::kLevelInvalid).message(), "CX_ERR_SCORING_LEVEL_INVALID");
}

}  // namespace
}  // namespace cortrix::scoring
