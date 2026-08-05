#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/retrieval/sparse_error.h"

// Sparse retrieval error model (§8, ARCH §4.1.11) — template A registry. Pins the 5 codes,
// their categories/retryability/retry_after_ms, structured_data contracts, and
// the Status bridge.
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

TEST(F40SparseErrorTest, CountIsFive) {
    EXPECT_EQ(kSparseErrorCodeCount, 5);
    EXPECT_EQ(std::size(kAll), 5u);
}

TEST(F40SparseErrorTest, CodeStringsMatchArchRegistry) {
    EXPECT_STREQ(SparseErrorCodeString(SparseErrorCode::kInferenceFailed),
                 "CX_ERR_F40_INFERENCE_FAILED");
    EXPECT_STREQ(SparseErrorCodeString(SparseErrorCode::kSparseSerializeFailed),
                 "CX_ERR_F40_SPARSE_SERIALIZE_FAILED");
    EXPECT_STREQ(SparseErrorCodeString(SparseErrorCode::kInvertedIndexWriteFailed),
                 "CX_ERR_F40_INVERTED_INDEX_WRITE_FAILED");
    EXPECT_STREQ(SparseErrorCodeString(SparseErrorCode::kSparseRetrieverFailed),
                 "CX_ERR_F40_SPARSE_RETRIEVER_FAILED");
    EXPECT_STREQ(SparseErrorCodeString(SparseErrorCode::kOnnxRuntimeInitFailed),
                 "CX_ERR_F40_ONNX_RUNTIME_INIT_FAILED");
}

TEST(F40SparseErrorTest, AllCodesUniqueAndPrefixed) {
    std::set<std::string> seen;
    for (auto code : kAll) {
        std::string s = SparseErrorCodeString(code);
        EXPECT_EQ(s.rfind("CX_ERR_F40_", 0), 0u) << s;
        EXPECT_TRUE(seen.insert(s).second) << "duplicate " << s;
    }
    EXPECT_EQ(seen.size(), 5u);
}

TEST(F40SparseErrorTest, CategoryAndRetryMatchDesign) {
    // §8 table.
    auto chk = [](SparseErrorCode c, ErrorCategory cat, bool retry,
                  std::optional<int> ms) {
        const auto& info = GetSparseErrorInfo(c);
        EXPECT_EQ(info.category, cat);
        EXPECT_EQ(info.retryable, retry);
        EXPECT_EQ(info.retry_after_ms, ms);
    };
    chk(SparseErrorCode::kInferenceFailed, ErrorCategory::kTransient, true, 500);
    chk(SparseErrorCode::kSparseSerializeFailed, ErrorCategory::kTransient, true, 100);
    chk(SparseErrorCode::kInvertedIndexWriteFailed, ErrorCategory::kTransient, true, 1000);
    chk(SparseErrorCode::kSparseRetrieverFailed, ErrorCategory::kTransient, true, 200);
    chk(SparseErrorCode::kOnnxRuntimeInitFailed, ErrorCategory::kPermanent, false,
        std::nullopt);
}

TEST(F40SparseErrorTest, RetryableImpliesRetryAfterSet) {
    for (auto code : kAll) {
        const auto& info = GetSparseErrorInfo(code);
        if (info.retryable) {
            EXPECT_TRUE(info.retry_after_ms.has_value())
                << SparseErrorCodeString(code);
        } else {
            EXPECT_FALSE(info.retry_after_ms.has_value());
        }
    }
}

TEST(F40SparseErrorTest, RequiredStructuredDataKeys) {
    EXPECT_EQ(RequiredStructuredDataKeys(SparseErrorCode::kInferenceFailed),
              (std::vector<std::string>{"chunk_id", "model"}));
    EXPECT_EQ(RequiredStructuredDataKeys(SparseErrorCode::kSparseSerializeFailed),
              (std::vector<std::string>{"chunk_id", "sparse_size"}));
    EXPECT_EQ(RequiredStructuredDataKeys(SparseErrorCode::kInvertedIndexWriteFailed),
              (std::vector<std::string>{"child_id", "term_count"}));
    EXPECT_EQ(RequiredStructuredDataKeys(SparseErrorCode::kSparseRetrieverFailed),
              (std::vector<std::string>{"ns_id"}));
    EXPECT_EQ(RequiredStructuredDataKeys(SparseErrorCode::kOnnxRuntimeInitFailed),
              (std::vector<std::string>{"model_path"}));
}

TEST(F40SparseErrorTest, HasRequiredStructuredData) {
    nlohmann::json full = {{"ns_id", "ns_01"}};
    EXPECT_TRUE(HasRequiredStructuredData(SparseErrorCode::kSparseRetrieverFailed, full));

    nlohmann::json missing = {{"other", 1}};
    EXPECT_FALSE(HasRequiredStructuredData(SparseErrorCode::kSparseRetrieverFailed, missing));

    nlohmann::json two = {{"child_id", "c1"}, {"term_count", 12}};
    EXPECT_TRUE(HasRequiredStructuredData(SparseErrorCode::kInvertedIndexWriteFailed, two));
}

TEST(F40SparseErrorTest, MakeSparseErrorFillsFromRegistry) {
    auto err = MakeSparseError(SparseErrorCode::kSparseRetrieverFailed,
                               {{"ns_id", "ns_01"}, {"fallback_used", true}});
    EXPECT_EQ(err.code, "CX_ERR_F40_SPARSE_RETRIEVER_FAILED");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 200);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["ns_id"], "ns_01");
}

TEST(F40SparseErrorTest, MakeSparseErrorDefaultMessageIsCode) {
    auto err = MakeSparseError(SparseErrorCode::kInferenceFailed);
    EXPECT_EQ(err.message, "CX_ERR_F40_INFERENCE_FAILED");
    auto err2 = MakeSparseError(SparseErrorCode::kInferenceFailed,
                                nlohmann::json::object(), "custom detail");
    EXPECT_EQ(err2.message, "custom detail");
}

TEST(F40SparseErrorTest, MakeSparseErrorSerializesToAgentFriendlyBody) {
    auto err = MakeSparseError(SparseErrorCode::kInvertedIndexWriteFailed,
                               {{"child_id", "c1"}, {"term_count", 7}});
    auto j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_F40_INVERTED_INDEX_WRITE_FAILED");
    EXPECT_EQ(j["retryable"], true);
    EXPECT_EQ(j["category"], "transient");
    EXPECT_EQ(j["retry_after_ms"], 1000);
    EXPECT_EQ(j["structured_data"]["child_id"], "c1");
}

TEST(F40SparseErrorTest, StatusBridgeCarriesTokenAndCode) {
    Status s = SparseStatus(SparseErrorCode::kOnnxRuntimeInitFailed, "bad path");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);  // permanent/operator-side
    EXPECT_NE(s.message().find("CX_ERR_F40_ONNX_RUNTIME_INIT_FAILED"),
              std::string::npos);
    EXPECT_NE(s.message().find("bad path"), std::string::npos);
}

TEST(F40SparseErrorTest, StatusCodeMapping) {
    EXPECT_EQ(SparseErrorToStatusCode(SparseErrorCode::kOnnxRuntimeInitFailed),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(SparseErrorToStatusCode(SparseErrorCode::kInferenceFailed),
              StatusCode::kUnavailable);
    EXPECT_EQ(SparseErrorToStatusCode(SparseErrorCode::kSparseRetrieverFailed),
              StatusCode::kUnavailable);
}

}  // namespace
}  // namespace cortrix::retrieval
