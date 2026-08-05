// S1.3 -- ONNX model load fail-fast + CX_ERR_RERANKER_INIT_FAILED + startup
// config-compat validation (CX_ERR_CONFIG_MISMATCH) + /health readiness contract
// + the RerankerErrorCode registry (reranker).
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/common/status.h"
#include "cortrix/reranker.h"
#include "cortrix/reranker/onnx_reranker.h"
#include "cortrix/reranker/reranker_error.h"
#include "cortrix/reranker/reranker_startup_validator.h"

namespace cortrix::reranker {
namespace {

using agent_friendly::ErrorCategory;

// --- RerankerErrorCode registry (reranker, mirrors the CatalogErrorCode test) ----------

TEST(RerankerErrorTest, AllFiveCodesHaveCanonicalCxStrings) {
    EXPECT_STREQ(RerankerErrorCodeString(RerankerErrorCode::kRerankerInitFailed),
                 "CX_ERR_RERANKER_INIT_FAILED");
    EXPECT_STREQ(RerankerErrorCodeString(RerankerErrorCode::kConfigMismatch),
                 "CX_ERR_CONFIG_MISMATCH");
    EXPECT_STREQ(RerankerErrorCodeString(RerankerErrorCode::kRerankerTaskTimeout),
                 "CX_ERR_RERANKER_TASK_TIMEOUT");
    EXPECT_STREQ(RerankerErrorCodeString(RerankerErrorCode::kChunkNotFound),
                 "CX_ERR_CHUNK_NOT_FOUND");
    EXPECT_STREQ(RerankerErrorCodeString(RerankerErrorCode::kRerankerCircuitOpen),
                 "CX_ERR_RERANKER_CIRCUIT_OPEN");
    EXPECT_EQ(kRerankerErrorCodeCount, 5);
}

TEST(RerankerErrorTest, CategoryAndRetryabilityMatchSpecTable) {
    // sec 5.2: INIT_FAILED permanent/false/null; CONFIG_MISMATCH permanent/false/null;
    // TASK_TIMEOUT timeout/true/5000; CHUNK_NOT_FOUND permanent/false/null;
    // CIRCUIT_OPEN transient/true/30000.
    const auto& init = GetRerankerErrorInfo(RerankerErrorCode::kRerankerInitFailed);
    EXPECT_EQ(init.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(init.retryable);
    EXPECT_FALSE(init.retry_after_ms.has_value());

    const auto& cfg = GetRerankerErrorInfo(RerankerErrorCode::kConfigMismatch);
    EXPECT_EQ(cfg.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(cfg.retryable);

    const auto& to = GetRerankerErrorInfo(RerankerErrorCode::kRerankerTaskTimeout);
    EXPECT_EQ(to.category, ErrorCategory::kTimeout);
    EXPECT_TRUE(to.retryable);
    ASSERT_TRUE(to.retry_after_ms.has_value());
    EXPECT_EQ(*to.retry_after_ms, 5000);

    const auto& co = GetRerankerErrorInfo(RerankerErrorCode::kRerankerCircuitOpen);
    EXPECT_EQ(co.category, ErrorCategory::kTransient);
    EXPECT_TRUE(co.retryable);
    ASSERT_TRUE(co.retry_after_ms.has_value());
    EXPECT_EQ(*co.retry_after_ms, 30000);
}

TEST(RerankerErrorTest, RequiredStructuredDataKeysMatchSpec) {
    EXPECT_EQ(RequiredStructuredDataKeys(RerankerErrorCode::kRerankerInitFailed),
              (std::vector<std::string>{"model_path", "ep"}));
    EXPECT_EQ(RequiredStructuredDataKeys(RerankerErrorCode::kConfigMismatch),
              (std::vector<std::string>{"chunk_size", "max_seq_length"}));
    EXPECT_EQ(RequiredStructuredDataKeys(RerankerErrorCode::kChunkNotFound),
              (std::vector<std::string>{"child_id"}));

    nlohmann::json good = {{"model_path", "/m"}, {"ep", "cpu"}};
    EXPECT_TRUE(HasRequiredStructuredData(RerankerErrorCode::kRerankerInitFailed, good));
    nlohmann::json missing = {{"model_path", "/m"}};
    EXPECT_FALSE(HasRequiredStructuredData(RerankerErrorCode::kRerankerInitFailed, missing));
}

TEST(RerankerErrorTest, MakeRerankerErrorFillsCanonicalFields) {
    auto err = MakeRerankerError(RerankerErrorCode::kRerankerCircuitOpen,
                                 {{"open_since", "2026-05-31T00:00:00Z"}, {"failure_count", 12}});
    EXPECT_EQ(err.code, "CX_ERR_RERANKER_CIRCUIT_OPEN");
    EXPECT_TRUE(err.retryable);
    EXPECT_EQ(err.category, ErrorCategory::kTransient);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 30000);
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_EQ((*err.structured_data)["failure_count"], 12);
}

TEST(RerankerErrorTest, StatusCarriesCxToken) {
    Status s = RerankerStatus(RerankerErrorCode::kConfigMismatch, "detail here");
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message().find("CX_ERR_CONFIG_MISMATCH"), std::string::npos);
}

// --- Startup config-compat validation (spc.chunk_size <= max_seq_length) ---

TEST(RerankerStartupValidatorTest, EqualOrSmallerChunkSizePasses) {
    EXPECT_TRUE(RerankerStartupValidator::ValidateConfigCompat(512, 512).ok());
    EXPECT_TRUE(RerankerStartupValidator::ValidateConfigCompat(256, 512).ok());
}

TEST(RerankerStartupValidatorTest, LargerChunkSizeFailsWithConfigMismatch) {
    Status s = RerankerStartupValidator::ValidateConfigCompat(1024, 512);
    EXPECT_FALSE(s.ok());
    // Both values + the CX token surface in the message (Agent-friendly detail).
    EXPECT_NE(s.message().find("CX_ERR_CONFIG_MISMATCH"), std::string::npos);
    EXPECT_NE(s.message().find("1024"), std::string::npos);
    EXPECT_NE(s.message().find("512"), std::string::npos);
}

// --- Fail-fast on a configured-but-bad model + /health readiness contract ---

TEST(OnnxRerankerInitTest, EmptyModelPathInitsStubAndIsReady) {
    RerankerConfig c;  // empty model_path = no model configured -> stub mode
    OnnxReranker r(c, nullptr);
    EXPECT_FALSE(r.is_ready());  // /health 503 before Init
    EXPECT_TRUE(r.Init().ok());
    EXPECT_TRUE(r.is_ready());   // /health 200 after Init
    EXPECT_FALSE(r.is_real_model());
}

TEST(OnnxRerankerInitTest, ConfiguredButMissingModelFailsFast) {
    RerankerConfig c;
    c.model_path = "/nonexistent/path/bge-reranker-v2-m3/model.onnx";
    OnnxReranker r(c, nullptr);
    Status s = r.Init();
    EXPECT_FALSE(s.ok());  // fail-fast (model configured but not present)
    EXPECT_NE(s.message().find("CX_ERR_RERANKER_INIT_FAILED"), std::string::npos);
    EXPECT_FALSE(r.is_ready());  // never becomes ready -> /health stays 503
}

TEST(OnnxRerankerInitTest, FactoryReturnsNullOnInitFailure) {
    RerankerConfig c;
    c.model_path = "/nonexistent/path/model.onnx";
    IReranker* r = CreateReranker(c, nullptr);
    EXPECT_EQ(r, nullptr);  // caller aborts startup on null
}

TEST(OnnxRerankerInitTest, ActiveEpReportsStubInStubMode) {
    RerankerConfig c;
    OnnxReranker r(c, nullptr);
    ASSERT_TRUE(r.Init().ok());
    EXPECT_STREQ(r.active_ep(), "stub");
}

// --- RerankerErrorCode registry: exhaustive per-code branch coverage ---

const RerankerErrorCode kAllRerankerCodes[] = {
    RerankerErrorCode::kRerankerInitFailed,  RerankerErrorCode::kConfigMismatch,
    RerankerErrorCode::kRerankerTaskTimeout, RerankerErrorCode::kChunkNotFound,
    RerankerErrorCode::kRerankerCircuitOpen,
};

// GetRerankerErrorInfo: every switch arm (incl. kChunkNotFound) returns a
// distinct, non-empty CX token; count anchor holds.
TEST(RerankerErrorBranchTest, GetInfoEveryArmDistinct) {
    EXPECT_EQ(kRerankerErrorCodeCount, 5);
    std::set<std::string> tokens;
    for (RerankerErrorCode c : kAllRerankerCodes) {
        const RerankerErrorInfo& info = GetRerankerErrorInfo(c);
        ASSERT_NE(info.cx_code, nullptr);
        tokens.insert(info.cx_code);
    }
    EXPECT_EQ(tokens.size(), 5u);
    // The kChunkNotFound arm (uncovered before): permanent / not retryable / null.
    const RerankerErrorInfo& chunk = GetRerankerErrorInfo(RerankerErrorCode::kChunkNotFound);
    EXPECT_EQ(std::string(chunk.cx_code), "CX_ERR_CHUNK_NOT_FOUND");
    EXPECT_EQ(chunk.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(chunk.retryable);
    EXPECT_FALSE(chunk.retry_after_ms.has_value());
}

// RequiredStructuredDataKeys: the timeout + circuit arms (uncovered before).
TEST(RerankerErrorBranchTest, RequiredKeysTimeoutAndCircuitArms) {
    EXPECT_EQ(RequiredStructuredDataKeys(RerankerErrorCode::kRerankerTaskTimeout),
              (std::vector<std::string>{"child_id", "timeout_ms"}));
    EXPECT_EQ(RequiredStructuredDataKeys(RerankerErrorCode::kRerankerCircuitOpen),
              (std::vector<std::string>{"open_since", "failure_count"}));
    // Every arm yields a non-empty key set.
    for (RerankerErrorCode c : kAllRerankerCodes) {
        EXPECT_FALSE(RequiredStructuredDataKeys(c).empty());
    }
}

// HasRequiredStructuredData: the non-object branch (returns keys.empty(), which is
// false for every reranker code since all carry required keys).
TEST(RerankerErrorBranchTest, HasRequiredNonObjectIsFalse) {
    EXPECT_FALSE(HasRequiredStructuredData(RerankerErrorCode::kChunkNotFound,
                                           nlohmann::json("a bare string")));
    // Object with the one required key present -> true.
    EXPECT_TRUE(HasRequiredStructuredData(RerankerErrorCode::kChunkNotFound,
                                          {{"child_id", "01ABC"}}));
}

// MakeRerankerError: the empty-message fallback arm (message := cx_code).
TEST(RerankerErrorBranchTest, MakeErrorEmptyMessageFallsBackToCode) {
    auto err = MakeRerankerError(RerankerErrorCode::kChunkNotFound);
    EXPECT_EQ(err.message, "CX_ERR_CHUNK_NOT_FOUND");
    EXPECT_FALSE(err.retryable);
    auto with_msg = MakeRerankerError(RerankerErrorCode::kChunkNotFound,
                                      nlohmann::json::object(), "explicit");
    EXPECT_EQ(with_msg.message, "explicit");
}

// RerankerErrorToStatusCode: every distinct mapping arm.
TEST(RerankerErrorBranchTest, ToStatusCodeEveryArm) {
    EXPECT_EQ(RerankerErrorToStatusCode(RerankerErrorCode::kRerankerInitFailed),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(RerankerErrorToStatusCode(RerankerErrorCode::kConfigMismatch),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(RerankerErrorToStatusCode(RerankerErrorCode::kChunkNotFound),
              StatusCode::kNotFound);
    EXPECT_EQ(RerankerErrorToStatusCode(RerankerErrorCode::kRerankerTaskTimeout),
              StatusCode::kUnavailable);
    EXPECT_EQ(RerankerErrorToStatusCode(RerankerErrorCode::kRerankerCircuitOpen),
              StatusCode::kUnavailable);
}

// RerankerStatus: the empty-detail arm (message == bare CX token).
TEST(RerankerErrorBranchTest, StatusEmptyDetailIsBareToken) {
    Status s = RerankerStatus(RerankerErrorCode::kChunkNotFound);  // no detail
    EXPECT_EQ(s.message(), "CX_ERR_CHUNK_NOT_FOUND");
    EXPECT_EQ(s.code(), StatusCode::kNotFound);
    Status d = RerankerStatus(RerankerErrorCode::kChunkNotFound, "child 01ABC");
    EXPECT_EQ(d.message(), "CX_ERR_CHUNK_NOT_FOUND: child 01ABC");
}

}  // namespace
}  // namespace cortrix::reranker
