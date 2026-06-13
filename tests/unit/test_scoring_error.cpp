#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/scoring/scoring_error.h"

// F07 S8 / §4.4 coverage: the 2 F07 error identities — CX_ERR_F07_* identity, category
// mapping, retryability, the GEN-Agent 4-field boundary factory, the structured_data
// contract, and the Status bridge. Mirrors tests/unit/test_import_error.cpp (template A).
namespace cortrix::scoring {
namespace {

using agent_friendly::ErrorCategory;

const std::vector<F07ErrorCode>& AllCodes() {
    static const std::vector<F07ErrorCode> codes = {
        F07ErrorCode::kLevelInvalid,
        F07ErrorCode::kConfigInvalid,
    };
    return codes;
}

TEST(F07ErrorTest, TwoCodesTotal) {
    EXPECT_EQ(AllCodes().size(), 2u);
    EXPECT_EQ(kF07ErrorCodeCount, 2);
}

TEST(F07ErrorTest, EveryCodeHasUniqueWellFormedCxString) {
    static const std::regex kPattern("^CX_ERR_F07_[A-Z][A-Z_]*$");
    std::set<std::string> seen;
    for (F07ErrorCode code : AllCodes()) {
        std::string cx = F07ErrorCodeString(code);
        EXPECT_TRUE(std::regex_match(cx, kPattern)) << cx;
        EXPECT_TRUE(seen.insert(cx).second) << "duplicate " << cx;
    }
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(std::string(F07ErrorCodeString(F07ErrorCode::kLevelInvalid)),
              "CX_ERR_F07_LEVEL_INVALID");
    EXPECT_EQ(std::string(F07ErrorCodeString(F07ErrorCode::kConfigInvalid)),
              "CX_ERR_F07_CONFIG_INVALID");
}

// §4.4: both permanent, neither retryable, retry_after_ms null.
TEST(F07ErrorTest, RegistryMatchesSpecTable) {
    for (F07ErrorCode code : AllCodes()) {
        const F07ErrorInfo& info = GetF07ErrorInfo(code);
        EXPECT_EQ(info.category, ErrorCategory::kPermanent) << F07ErrorCodeString(code);
        EXPECT_FALSE(info.retryable) << F07ErrorCodeString(code);
        EXPECT_FALSE(info.retry_after_ms.has_value()) << F07ErrorCodeString(code);
    }
}

// MakeScoringError fills the 4 GEN-Agent fields from the registry + serializes to the
// §3.1 error body.
TEST(F07ErrorTest, MakeErrorPopulatesAgentFriendlyFields) {
    auto err = MakeScoringError(
        F07ErrorCode::kConfigInvalid,
        {{"alpha_received", 1.5}, {"valid_range", {0.0, 1.0}},
         {"config_source", "config.yaml:scoring.alpha"}},
        "alpha out of [0,1]");
    EXPECT_EQ(err.code, "CX_ERR_F07_CONFIG_INVALID");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["config_source"], "config.yaml:scoring.alpha");

    auto body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_F07_CONFIG_INVALID");
    EXPECT_EQ(body["category"], "permanent");
    EXPECT_TRUE(body["retry_after_ms"].is_null());
}

TEST(F07ErrorTest, EmptyMessageFallsBackToCode) {
    auto err = MakeScoringError(F07ErrorCode::kLevelInvalid);
    EXPECT_EQ(err.message, "CX_ERR_F07_LEVEL_INVALID");
}

// §4.4 structured_data contract (GEN-Agent #5).
TEST(F07ErrorTest, RequiredStructuredDataContract) {
    nlohmann::json level_full = {
        {"level_received", 7}, {"max_allowed", 4}, {"scoring_input", nullptr}};
    EXPECT_TRUE(HasRequiredStructuredData(F07ErrorCode::kLevelInvalid, level_full));
    EXPECT_FALSE(HasRequiredStructuredData(F07ErrorCode::kLevelInvalid,
                                           nlohmann::json{{"level_received", 7}}));

    nlohmann::json cfg_full = {
        {"alpha_received", 1.5}, {"valid_range", {0.0, 1.0}}, {"config_source", "x"}};
    EXPECT_TRUE(HasRequiredStructuredData(F07ErrorCode::kConfigInvalid, cfg_full));
    EXPECT_FALSE(HasRequiredStructuredData(F07ErrorCode::kConfigInvalid, nlohmann::json("x")));
}

// ScoringStatus bridges to a coarse Status whose message recovers the exact identity.
TEST(F07ErrorTest, StatusBridgePreservesIdentityInMessage) {
    Status s = ScoringStatus(F07ErrorCode::kLevelInvalid, "level 7");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(s.message().find("CX_ERR_F07_LEVEL_INVALID"), std::string::npos);
    EXPECT_NE(s.message().find("level 7"), std::string::npos);

    EXPECT_EQ(ScoringStatus(F07ErrorCode::kConfigInvalid).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(ScoringStatus(F07ErrorCode::kLevelInvalid).message(), "CX_ERR_F07_LEVEL_INVALID");
}

}  // namespace
}  // namespace cortrix::scoring
