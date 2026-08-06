#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/onnx/onnx_error.h"

// Exhaustive parameterized error-registry sweep for ONNX (codes).
// Distinct suite name (OnnxErrorMatrix) from test_onnx_error.cpp.
namespace cortrix::onnx {
namespace {

using agent_friendly::ErrorCategory;

const OnnxErrorCode kAll[] = {
    OnnxErrorCode::kRuntimeVersionMismatch,
    OnnxErrorCode::kOpsetIncompatible,
    OnnxErrorCode::kInferenceFailed,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class OnnxErrorMatrix : public ::testing::TestWithParam<OnnxErrorCode> {};

TEST_P(OnnxErrorMatrix, CodeStringFormat) {
    const OnnxErrorCode code = GetParam();
    const std::string s = OnnxErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetOnnxErrorInfo(code).cx_code, s.c_str());
}

TEST_P(OnnxErrorMatrix, CategoryAndRetryInvariant) {
    const OnnxErrorCode code = GetParam();
    const OnnxErrorInfo& info = GetOnnxErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(OnnxErrorMatrix, RequiredStructuredDataArms) {
    const OnnxErrorCode code = GetParam();
    const std::vector<std::string>& keys = RequiredStructuredDataKeys(code);

    nlohmann::json full = nlohmann::json::object();
    for (const std::string& k : keys) full[k] = "v";
    EXPECT_TRUE(HasRequiredStructuredData(code, full));

    if (!keys.empty()) {
        nlohmann::json missing = full;
        missing.erase(keys.front());
        EXPECT_FALSE(HasRequiredStructuredData(code, missing));
    }

    EXPECT_EQ(HasRequiredStructuredData(code, nlohmann::json("not-an-object")),
              keys.empty());
}

TEST_P(OnnxErrorMatrix, MakeRoundTripAndJson) {
    const OnnxErrorCode code = GetParam();
    const OnnxErrorInfo& info = GetOnnxErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeOnnxError(code, sd, "human detail");
    EXPECT_EQ(err.code, info.cx_code);
    EXPECT_EQ(err.message, "human detail");
    EXPECT_EQ(err.retryable, info.retryable);
    EXPECT_EQ(err.category, info.category);
    EXPECT_EQ(err.retry_after_ms, info.retry_after_ms);

    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], std::string(info.cx_code));
    EXPECT_EQ(j["retryable"], info.retryable);
    EXPECT_EQ(j["category"], std::string(agent_friendly::ToString(info.category)));
    EXPECT_EQ(j["retry_after_ms"].is_null(), !info.retry_after_ms.has_value());
}

TEST_P(OnnxErrorMatrix, EmptyMessageFallsBackToCode) {
    const OnnxErrorCode code = GetParam();
    auto err = MakeOnnxError(code);
    EXPECT_EQ(err.message, std::string(OnnxErrorCodeString(code)));
}

TEST_P(OnnxErrorMatrix, StatusBridge) {
    const OnnxErrorCode code = GetParam();
    const StatusCode mapped = OnnxErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = OnnxStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(OnnxErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(OnnxErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kOnnxErrorCodeCount, 3);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kOnnxErrorCodeCount));
    std::set<std::string> seen;
    for (OnnxErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(OnnxErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kOnnxErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, OnnxErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::onnx
