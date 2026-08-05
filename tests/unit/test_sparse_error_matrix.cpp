#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <vector>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/retrieval/sparse_error.h"

// Exhaustive parameterized error-registry sweep for sparse (§8, 5 codes).
// Distinct suite name (SparseErrorMatrix) from test_f40_sparse_error.cpp.
namespace cortrix::retrieval {
namespace {

using agent_friendly::ErrorCategory;

const SparseErrorCode kAll[] = {
    SparseErrorCode::kInferenceFailed,
    SparseErrorCode::kSparseSerializeFailed,
    SparseErrorCode::kInvertedIndexWriteFailed,
    SparseErrorCode::kSparseRetrieverFailed,
    SparseErrorCode::kOnnxRuntimeInitFailed,
};

bool IsValidCategory(ErrorCategory c) {
    return c == ErrorCategory::kAuth || c == ErrorCategory::kQuota ||
           c == ErrorCategory::kTransient || c == ErrorCategory::kPermanent ||
           c == ErrorCategory::kTimeout;
}

class SparseErrorMatrix : public ::testing::TestWithParam<SparseErrorCode> {};

TEST_P(SparseErrorMatrix, CodeStringFormat) {
    const SparseErrorCode code = GetParam();
    const std::string s = SparseErrorCodeString(code);
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.rfind("CX_ERR_SPARSE_", 0), 0u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex("^CX_ERR_SPARSE_[A-Z0-9_]+$"))) << s;
    EXPECT_STREQ(GetSparseErrorInfo(code).cx_code, s.c_str());
}

TEST_P(SparseErrorMatrix, CategoryAndRetryInvariant) {
    const SparseErrorCode code = GetParam();
    const SparseErrorInfo& info = GetSparseErrorInfo(code);
    EXPECT_TRUE(IsValidCategory(info.category));
    if (!info.retryable) {
        EXPECT_FALSE(info.retry_after_ms.has_value()) << info.cx_code;
    }
    if (info.retry_after_ms.has_value()) {
        EXPECT_GT(*info.retry_after_ms, 0) << info.cx_code;
    }
}

TEST_P(SparseErrorMatrix, RequiredStructuredDataArms) {
    const SparseErrorCode code = GetParam();
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

TEST_P(SparseErrorMatrix, MakeRoundTripAndJson) {
    const SparseErrorCode code = GetParam();
    const SparseErrorInfo& info = GetSparseErrorInfo(code);

    nlohmann::json sd = nlohmann::json::object();
    for (const std::string& k : RequiredStructuredDataKeys(code)) sd[k] = "v";

    auto err = MakeSparseError(code, sd, "human detail");
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

TEST_P(SparseErrorMatrix, EmptyMessageFallsBackToCode) {
    const SparseErrorCode code = GetParam();
    auto err = MakeSparseError(code);
    EXPECT_EQ(err.message, std::string(SparseErrorCodeString(code)));
}

TEST_P(SparseErrorMatrix, StatusBridge) {
    const SparseErrorCode code = GetParam();
    const StatusCode mapped = SparseErrorToStatusCode(code);
    EXPECT_NE(mapped, StatusCode::kOk);

    Status s = SparseStatus(code, "detail-x");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), mapped);
    EXPECT_NE(s.message().find(SparseErrorCodeString(code)), std::string::npos);
    EXPECT_NE(s.message().find("detail-x"), std::string::npos);
}

TEST(SparseErrorMatrixAggregate, CountAndUniqueCodes) {
    EXPECT_EQ(kSparseErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kSparseErrorCodeCount));
    std::set<std::string> seen;
    for (SparseErrorCode c : kAll) {
        EXPECT_TRUE(seen.insert(SparseErrorCodeString(c)).second);
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kSparseErrorCodeCount));
}

INSTANTIATE_TEST_SUITE_P(AllCodes, SparseErrorMatrix, ::testing::ValuesIn(kAll));

}  // namespace
}  // namespace cortrix::retrieval
