#include <gtest/gtest.h>

#include <array>
#include <string>

#include <nlohmann/json.hpp>

#include "cortrix/agent_friendly/error.h"
#include "cortrix/onnx/onnx_error.h"

namespace cortrix::onnx {
namespace {

using agent_friendly::ErrorCategory;

constexpr std::array<OnnxErrorCode, kOnnxErrorCodeCount> kAllCodes = {
    OnnxErrorCode::kRuntimeVersionMismatch,
    OnnxErrorCode::kOpsetIncompatible,
    OnnxErrorCode::kInferenceFailed,
};

// ============================================================
// Registry: stable CX_ERR_* strings
// ============================================================

TEST(OnnxErrorTest, CodeStrings) {
    EXPECT_STREQ(OnnxErrorCodeString(OnnxErrorCode::kRuntimeVersionMismatch),
                 "CX_ERR_ONNXRT_VERSION_MISMATCH");
    EXPECT_STREQ(OnnxErrorCodeString(OnnxErrorCode::kOpsetIncompatible),
                 "CX_ERR_ONNX_OPSET_INCOMPATIBLE");
    EXPECT_STREQ(OnnxErrorCodeString(OnnxErrorCode::kInferenceFailed),
                 "CX_ERR_ONNX_INFERENCE_FAILED");
}

// API-compatibility anchor: the set must not shrink (GEN-Agent #7).
TEST(OnnxErrorTest, CodeCountIsThree) {
    EXPECT_EQ(kOnnxErrorCodeCount, 3);
}

// Every code resolves to a non-empty, unique CX_ERR_* string.
TEST(OnnxErrorTest, AllCodesHaveUniqueStrings) {
    std::array<std::string, kOnnxErrorCodeCount> seen{};
    std::size_t n = 0;
    for (OnnxErrorCode code : kAllCodes) {
        const char* s = OnnxErrorCodeString(code);
        ASSERT_NE(s, nullptr);
        EXPECT_FALSE(std::string(s).empty());
        for (std::size_t i = 0; i < n; ++i) {
            EXPECT_NE(seen[i], std::string(s)) << "duplicate CX_ERR_ string";
        }
        seen[n++] = s;
    }
}

// ============================================================
// Registry: category + retryability
// ============================================================

TEST(OnnxErrorTest, StartupErrorsArePermanentNonRetryable) {
    for (OnnxErrorCode code :
         {OnnxErrorCode::kRuntimeVersionMismatch, OnnxErrorCode::kOpsetIncompatible}) {
        const OnnxErrorInfo& info = GetOnnxErrorInfo(code);
        EXPECT_EQ(info.category, ErrorCategory::kPermanent);
        EXPECT_FALSE(info.retryable);
        EXPECT_FALSE(info.retry_after_ms.has_value());
    }
}

TEST(OnnxErrorTest, InferenceFailedIsTransientRetryable200ms) {
    const OnnxErrorInfo& info = GetOnnxErrorInfo(OnnxErrorCode::kInferenceFailed);
    EXPECT_EQ(info.category, ErrorCategory::kTransient);
    EXPECT_TRUE(info.retryable);
    ASSERT_TRUE(info.retry_after_ms.has_value());
    EXPECT_EQ(*info.retry_after_ms, 200);
}

// ============================================================
// Required structured_data keys (ONNX runtime — Agent-friendly #5)
// ============================================================

TEST(OnnxErrorTest, RequiredKeysPerCode) {
    EXPECT_EQ(RequiredStructuredDataKeys(OnnxErrorCode::kRuntimeVersionMismatch),
              (std::vector<std::string>{"current_version", "expected_major_version", "action"}));
    EXPECT_EQ(RequiredStructuredDataKeys(OnnxErrorCode::kOpsetIncompatible),
              (std::vector<std::string>{"model_path", "model_opset", "supported_opset_range", "action"}));
    EXPECT_EQ(RequiredStructuredDataKeys(OnnxErrorCode::kInferenceFailed),
              (std::vector<std::string>{"onnx_error_code", "op_kernel", "last_input_shape"}));
}

TEST(OnnxErrorTest, HasRequiredStructuredData_Complete) {
    nlohmann::json sd = {
        {"current_version", "2.0.1"},
        {"expected_major_version", "1"},
        {"action", "replace_so_or_rebuild"},
    };
    EXPECT_TRUE(HasRequiredStructuredData(OnnxErrorCode::kRuntimeVersionMismatch, sd));
}

TEST(OnnxErrorTest, HasRequiredStructuredData_MissingKey) {
    nlohmann::json sd = {
        {"current_version", "2.0.1"},
        {"action", "replace_so_or_rebuild"},
        // missing "expected_major_version"
    };
    EXPECT_FALSE(HasRequiredStructuredData(OnnxErrorCode::kRuntimeVersionMismatch, sd));
}

TEST(OnnxErrorTest, HasRequiredStructuredData_NonObjectFails) {
    // A non-object payload can't carry the required keys → false (none of the 3
    // codes have an empty required-key set).
    EXPECT_FALSE(HasRequiredStructuredData(OnnxErrorCode::kInferenceFailed,
                                           nlohmann::json("not an object")));
}

// ============================================================
// MakeOnnxError → AgentFriendlyError (ONNX runtime / §9.1)
// ============================================================

TEST(OnnxErrorTest, MakeOnnxError_FillsRegistryFields) {
    nlohmann::json sd = {
        {"model_path", "/models/bge-reranker-v2-m3.onnx"},
        {"model_opset", 20},
        {"supported_opset_range", {10, 19}},
        {"action", "downgrade_model_or_upgrade_runtime"},
    };
    auto err = MakeOnnxError(OnnxErrorCode::kOpsetIncompatible, sd);

    EXPECT_EQ(err.code, "CX_ERR_ONNX_OPSET_INCOMPATIBLE");
    EXPECT_EQ(err.category, ErrorCategory::kPermanent);
    EXPECT_FALSE(err.retryable);
    EXPECT_FALSE(err.retry_after_ms.has_value());
    // Default message falls back to the code when none supplied.
    EXPECT_EQ(err.message, "CX_ERR_ONNX_OPSET_INCOMPATIBLE");
}

TEST(OnnxErrorTest, MakeOnnxError_CustomMessageAndRetryAfter) {
    auto err = MakeOnnxError(OnnxErrorCode::kInferenceFailed,
                             {{"onnx_error_code", "ORT_RUNTIME_EXCEPTION"},
                              {"op_kernel", "MatMul"},
                              {"last_input_shape", {1, 512, 768}}},
                             "inference failed after 1 retry");
    EXPECT_EQ(err.message, "inference failed after 1 retry");
    EXPECT_TRUE(err.retryable);
    ASSERT_TRUE(err.retry_after_ms.has_value());
    EXPECT_EQ(*err.retry_after_ms, 200);
}

// TC-14: structured_data is a JSON OBJECT, not a serialized string — Agent
// consumes it directly with no JSON.parse (ONNX runtime Anti-pattern 4).
TEST(OnnxErrorTest, StructuredDataIsObjectNotString) {
    auto err = MakeOnnxError(OnnxErrorCode::kRuntimeVersionMismatch,
                             {{"current_version", "2.0.1"},
                              {"expected_major_version", "1"},
                              {"action", "replace_so_or_rebuild"}});
    ASSERT_TRUE(err.structured_data.has_value());
    EXPECT_TRUE(err.structured_data->is_object());
    EXPECT_FALSE(err.structured_data->is_string());
    EXPECT_EQ((*err.structured_data)["expected_major_version"], "1");
}

// Full Agent-friendly body round-trips through ToJson with structured_data
// staying a nested object.
TEST(OnnxErrorTest, ToJsonKeepsStructuredDataNested) {
    auto err = MakeOnnxError(OnnxErrorCode::kOpsetIncompatible,
                             {{"model_path", "/models/bge-m3.onnx"},
                              {"model_opset", 99},
                              {"supported_opset_range", {7, 19}},
                              {"action", "downgrade_model_or_upgrade_runtime"}});
    nlohmann::json body = agent_friendly::ToJson(err);
    EXPECT_EQ(body["code"], "CX_ERR_ONNX_OPSET_INCOMPATIBLE");
    EXPECT_EQ(body["category"], "permanent");
    EXPECT_EQ(body["retryable"], false);
    ASSERT_TRUE(body["structured_data"].is_object());
    EXPECT_EQ(body["structured_data"]["model_opset"], 99);
}

// ============================================================
// OnnxStatus bridge (F-FREEZE-1: Result<T>/Status surface)
// ============================================================

TEST(OnnxErrorTest, OnnxStatus_CarriesCodeTokenInMessage) {
    Status s = OnnxStatus(OnnxErrorCode::kInferenceFailed, "MatMul failed");
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kUnavailable);
    EXPECT_NE(s.message().find("CX_ERR_ONNX_INFERENCE_FAILED"), std::string::npos);
    EXPECT_NE(s.message().find("MatMul failed"), std::string::npos);
}

TEST(OnnxErrorTest, OnnxStatus_StartupErrorsMapToInvalidArgument) {
    EXPECT_EQ(OnnxStatus(OnnxErrorCode::kRuntimeVersionMismatch).code(),
              StatusCode::kInvalidArgument);
    EXPECT_EQ(OnnxStatus(OnnxErrorCode::kOpsetIncompatible).code(),
              StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace cortrix::onnx
