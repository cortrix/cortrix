#include "cortrix/retrieval/sparse_error.h"

#include <utility>

namespace cortrix::retrieval {

using agent_friendly::AgentFriendlyError;
using agent_friendly::ErrorCategory;

namespace {

// One canonical row per code. Function-local statics → stable refs. The
// switches below are intentionally exhaustive: -Wswitch turns "added a code
// without a row" into a build failure, so the registry can't drift from the enum.
//
// retry_after_ms (table): INFERENCE_FAILED 500 / SERIALIZE 100 / INDEX_WRITE
// 1000 / RETRIEVER 200 (all transient/retryable); ONNX_RUNTIME_INIT is permanent
// (operator must fix the model/path) → not retryable.
constexpr SparseErrorInfo kInferenceFailed{
    "CX_ERR_SPARSE_INFERENCE_FAILED", ErrorCategory::kTransient, true, 500};
constexpr SparseErrorInfo kSparseSerializeFailed{
    "CX_ERR_SPARSE_SERIALIZE_FAILED", ErrorCategory::kTransient, true, 100};
constexpr SparseErrorInfo kInvertedIndexWriteFailed{
    "CX_ERR_SPARSE_INVERTED_INDEX_WRITE_FAILED", ErrorCategory::kTransient, true, 1000};
constexpr SparseErrorInfo kSparseRetrieverFailed{
    "CX_ERR_SPARSE_RETRIEVER_FAILED", ErrorCategory::kTransient, true, 200};
constexpr SparseErrorInfo kOnnxRuntimeInitFailed{
    "CX_ERR_SPARSE_ONNX_RUNTIME_INIT_FAILED", ErrorCategory::kPermanent, false, std::nullopt};

}  // namespace

const SparseErrorInfo& GetSparseErrorInfo(SparseErrorCode code) {
    switch (code) {
        case SparseErrorCode::kInferenceFailed:          return kInferenceFailed;
        case SparseErrorCode::kSparseSerializeFailed:    return kSparseSerializeFailed;
        case SparseErrorCode::kInvertedIndexWriteFailed: return kInvertedIndexWriteFailed;
        case SparseErrorCode::kSparseRetrieverFailed:    return kSparseRetrieverFailed;
        case SparseErrorCode::kOnnxRuntimeInitFailed:    return kOnnxRuntimeInitFailed;
    }
    return kInferenceFailed;  // unreachable for a valid enum value
}

const char* SparseErrorCodeString(SparseErrorCode code) {
    return GetSparseErrorInfo(code).cx_code;
}

const std::vector<std::string>& RequiredStructuredDataKeys(SparseErrorCode code) {
    // structured_data column, 1:1. Function-local statics → stable references.
    static const std::vector<std::string> kInfer{"chunk_id", "model"};
    static const std::vector<std::string> kSer{"chunk_id", "sparse_size"};
    static const std::vector<std::string> kIdxWrite{"child_id", "term_count"};
    static const std::vector<std::string> kRetriever{"ns_id"};
    static const std::vector<std::string> kInit{"model_path"};

    switch (code) {
        case SparseErrorCode::kInferenceFailed:          return kInfer;
        case SparseErrorCode::kSparseSerializeFailed:    return kSer;
        case SparseErrorCode::kInvertedIndexWriteFailed: return kIdxWrite;
        case SparseErrorCode::kSparseRetrieverFailed:    return kRetriever;
        case SparseErrorCode::kOnnxRuntimeInitFailed:    return kInit;
    }
    return kInfer;  // unreachable for a valid enum
}

bool HasRequiredStructuredData(SparseErrorCode code,
                               const nlohmann::json& structured_data) {
    if (!structured_data.is_object()) {
        return RequiredStructuredDataKeys(code).empty();
    }
    for (const std::string& key : RequiredStructuredDataKeys(code)) {
        if (!structured_data.contains(key)) return false;
    }
    return true;
}

AgentFriendlyError MakeSparseError(SparseErrorCode code,
                                   nlohmann::json structured_data,
                                   const std::string& message) {
    const SparseErrorInfo& info = GetSparseErrorInfo(code);
    AgentFriendlyError err;
    err.code = info.cx_code;
    err.message = message.empty() ? info.cx_code : message;
    err.retryable = info.retryable;
    err.category = info.category;
    err.retry_after_ms = info.retry_after_ms;
    err.structured_data = std::move(structured_data);
    return err;
}

StatusCode SparseErrorToStatusCode(SparseErrorCode code) {
    switch (code) {
        // Startup model load is permanent, operator-side (bad path / model) →
        // kInvalidArgument. The rich CX_ERR_* identity survives via the token +
        // boundary MakeSparseError().
        case SparseErrorCode::kOnnxRuntimeInitFailed:
            return StatusCode::kInvalidArgument;
        // Inference / serialize / index-write / retriever faults are transient
        // server-side → kUnavailable (retryable, with retry_after_ms).
        case SparseErrorCode::kInferenceFailed:
        case SparseErrorCode::kSparseSerializeFailed:
        case SparseErrorCode::kInvertedIndexWriteFailed:
        case SparseErrorCode::kSparseRetrieverFailed:
            return StatusCode::kUnavailable;
    }
    return StatusCode::kInternal;  // unreachable for a valid enum
}

Status SparseStatus(SparseErrorCode code, const std::string& detail) {
    const char* cx = SparseErrorCodeString(code);
    std::string msg = detail.empty() ? std::string(cx)
                                     : std::string(cx) + ": " + detail;
    return Status(SparseErrorToStatusCode(code), std::move(msg));
}

}  // namespace cortrix::retrieval
