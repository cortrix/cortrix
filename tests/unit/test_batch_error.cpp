#include <gtest/gtest.h>

#include <set>
#include <string>

#include "cortrix/server/batch_error.h"

// Batch-submit coverage: the batch error model (template A, mirrors async_error) —
// all 4 CX_ERR_BATCH_* identities, their §2.4.1 attributes
// (http/category/retryable/retry_after_ms/structured_data keys), and the
// AgentFriendlyError builder.
namespace cortrix::server {
namespace {

using agent_friendly::ErrorCategory;

// Every enum value the suite walks (kept in sync with kBatchErrorCodeCount).
constexpr BatchErrorCode kAll[] = {
    BatchErrorCode::kSizeExceeded,
    BatchErrorCode::kPayloadTooLarge,
    BatchErrorCode::kEmpty,
    BatchErrorCode::kDuplicateDocId,
};

TEST(BatchErrorTest, CountMatchesEnumeration) {
    EXPECT_EQ(kBatchErrorCodeCount, 4);
    EXPECT_EQ(std::size(kAll), static_cast<size_t>(kBatchErrorCodeCount));
}

TEST(BatchErrorTest, AllCodesHaveUniqueCxBatchStrings) {
    std::set<std::string> seen;
    for (BatchErrorCode c : kAll) {
        std::string s = BatchErrorCodeString(c);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("CX_ERR_BATCH_", 0), 0u) << s << " must start with CX_ERR_BATCH_";
        EXPECT_TRUE(seen.insert(s).second) << "duplicate code string: " << s;
    }
    EXPECT_EQ(seen.size(), 4u);
}

// §2.4.1 table, row by row.
TEST(BatchErrorTest, RegistryMatchesSpecTable) {
    auto chk = [](BatchErrorCode c, const char* code, int http, ErrorCategory cat,
                  bool retry, std::optional<int> retry_ms) {
        const BatchErrorInfo& i = GetBatchErrorInfo(c);
        EXPECT_STREQ(i.cx_code, code);
        EXPECT_EQ(i.http_status, http) << code;
        EXPECT_EQ(i.category, cat) << code;
        EXPECT_EQ(i.retryable, retry) << code;
        EXPECT_EQ(i.retry_after_ms, retry_ms) << code;
    };
    chk(BatchErrorCode::kSizeExceeded, "CX_ERR_BATCH_SIZE_EXCEEDED", 400,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(BatchErrorCode::kPayloadTooLarge, "CX_ERR_BATCH_PAYLOAD_TOO_LARGE", 413,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(BatchErrorCode::kEmpty, "CX_ERR_BATCH_EMPTY", 400,
        ErrorCategory::kPermanent, false, std::nullopt);
    chk(BatchErrorCode::kDuplicateDocId, "CX_ERR_BATCH_DUPLICATE_DOC_ID", 400,
        ErrorCategory::kPermanent, false, std::nullopt);
}

TEST(BatchErrorTest, AllBatchLevelFaultsArePermanentAndNonRetryable) {
    // §2.4.1: every batch-envelope fault rejects the whole request and is not
    // retryable (no retry_after_ms). GEN-Agent #6 consistency.
    for (BatchErrorCode c : kAll) {
        const BatchErrorInfo& i = GetBatchErrorInfo(c);
        EXPECT_EQ(i.category, ErrorCategory::kPermanent) << i.cx_code;
        EXPECT_FALSE(i.retryable) << i.cx_code;
        EXPECT_FALSE(i.retry_after_ms.has_value()) << i.cx_code;
    }
}

TEST(BatchErrorTest, HttpStatusAccessor) {
    EXPECT_EQ(BatchErrorHttpStatus(BatchErrorCode::kSizeExceeded), 400);
    EXPECT_EQ(BatchErrorHttpStatus(BatchErrorCode::kPayloadTooLarge), 413);
    EXPECT_EQ(BatchErrorHttpStatus(BatchErrorCode::kEmpty), 400);
    EXPECT_EQ(BatchErrorHttpStatus(BatchErrorCode::kDuplicateDocId), 400);
}

TEST(BatchErrorTest, RequiredStructuredDataKeysPerSpec) {
    EXPECT_EQ(BatchRequiredStructuredDataKeys(BatchErrorCode::kSizeExceeded),
              (std::vector<std::string>{"max_size", "actual_size"}));
    EXPECT_EQ(BatchRequiredStructuredDataKeys(BatchErrorCode::kPayloadTooLarge),
              (std::vector<std::string>{"max_bytes", "actual_bytes"}));
    // BATCH_EMPTY → {} (no required keys).
    EXPECT_TRUE(BatchRequiredStructuredDataKeys(BatchErrorCode::kEmpty).empty());
    EXPECT_EQ(BatchRequiredStructuredDataKeys(BatchErrorCode::kDuplicateDocId),
              (std::vector<std::string>{"duplicate_doc_ids"}));
}

TEST(BatchErrorTest, HasRequiredStructuredDataValidatesKeys) {
    nlohmann::json full = {{"max_size", 100}, {"actual_size", 250}};
    EXPECT_TRUE(BatchHasRequiredStructuredData(BatchErrorCode::kSizeExceeded, full));

    nlohmann::json missing = {{"max_size", 100}};
    EXPECT_FALSE(BatchHasRequiredStructuredData(BatchErrorCode::kSizeExceeded, missing));

    // A code with no required keys accepts an empty object.
    EXPECT_TRUE(BatchHasRequiredStructuredData(BatchErrorCode::kEmpty,
                                               nlohmann::json::object()));
    // Non-object payload only passes when no keys are required.
    EXPECT_FALSE(BatchHasRequiredStructuredData(BatchErrorCode::kSizeExceeded,
                                                nlohmann::json("not-an-object")));
    EXPECT_TRUE(BatchHasRequiredStructuredData(BatchErrorCode::kEmpty,
                                               nlohmann::json("anything")));
}

TEST(BatchErrorTest, MakeBatchErrorFillsFromRegistry) {
    nlohmann::json sd = {{"max_size", 100}, {"actual_size", 137}};
    auto err = MakeBatchError(BatchErrorCode::kSizeExceeded, sd,
                              "batch exceeds the 100-document limit");
    EXPECT_EQ(err.code, "CX_ERR_BATCH_SIZE_EXCEEDED");
    EXPECT_EQ(err.message, "batch exceeds the 100-document limit");
    EXPECT_FALSE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["actual_size"], 137);
}

TEST(BatchErrorTest, MakeBatchErrorDefaultsMessageToCode) {
    auto err = MakeBatchError(BatchErrorCode::kEmpty);
    EXPECT_EQ(err.message, "CX_ERR_BATCH_EMPTY");
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
}

TEST(BatchErrorTest, ToJsonSerializesAgentFriendlyBody) {
    auto err = MakeBatchError(BatchErrorCode::kDuplicateDocId,
                              {{"duplicate_doc_ids", {"doc_001", "doc_001"}}});
    nlohmann::json j = agent_friendly::ToJson(err);
    EXPECT_EQ(j["code"], "CX_ERR_BATCH_DUPLICATE_DOC_ID");
    EXPECT_EQ(j["retryable"], false);
    EXPECT_EQ(j["category"], "permanent");
    EXPECT_TRUE(j["retry_after_ms"].is_null());
    ASSERT_TRUE(j["structured_data"]["duplicate_doc_ids"].is_array());
    EXPECT_EQ(j["structured_data"]["duplicate_doc_ids"][0], "doc_001");
}

}  // namespace
}  // namespace cortrix::server
