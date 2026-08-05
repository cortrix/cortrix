#pragma once
#include <string>
#include <vector>

#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/observability/trace_context.h"

namespace cortrix {
struct CortrixConfig;  // fwd-decl; CollectRegisteredOnnxModels is defined in the .cpp
}

namespace cortrix::onnx {

/// Startup-time ONNX validator (fail-fast at startup). Run once at
/// cortrix-server startup, before SPC pipeline init (and before catalog schema
/// migration), so an incompatible ONNX Runtime / model is caught in the window
/// the operator is watching — not at the first inference request in prod.
///
/// Validates two things against the loaded runtime (cortrix::onnx::Runtime):
///   1. the runtime's ABI major version matches the CMake-compiled expectation
///      (else CX_ERR_ONNXRT_VERSION_MISMATCH);
///   2. every registered model's ONNX opset is within the runtime's supported
///      range (else CX_ERR_ONNX_OPSET_INCOMPATIBLE).
///
/// On failure it returns a Status carrying the CX_ERR_* identity (re-inflatable
/// to the full Agent-friendly body via MakeOnnxError); main() aborts on a
/// non-OK Status (hard-fail, D3=A — no degrade, no fallback).
///
/// Standalone (D3): this class validates a *given* list of model paths. Wiring
/// it into the live cortrix-server startup and populating the list from the reranker's
/// reranker_config + the live OnnxEmbedder config is cross-Feature integration
/// → deferred to D3.5 (see collect_registered_onnx_models, not built here).
class StartupValidator {
 public:
    struct ValidationConfig {
        /// Absolute paths of the .onnx models registered for this instance.
        /// Populated from reranker_config.model_path (bge-reranker
        /// -v2-m3) + OnnxEmbedder model_path (bge-m3) + sparse retrieval (future).
        std::vector<std::string> registered_model_paths;

        /// When true, a registered path that does not exist on disk is skipped
        /// (not an error). This matches the MVP OnnxEmbedder behavior, where a
        /// missing model falls back to stub embeddings rather than aborting — so
        /// a dev box without the model files still starts. Default false:
        /// startup validation treats a missing registered model as a hard error
        /// (the production contract — if it's registered it must be present).
        bool skip_missing_models = false;
    };

    /// Per-model opset diagnostic, surfaced for the --check-onnx dry-run report
    /// (S3) and for tests. Populated for every model that was read.
    struct ModelOpsetInfo {
        std::string model_path;
        int model_opset = 0;
        bool checked = false;   ///< false if skipped (missing + skip_missing_models)
    };

    /// Full validation outcome (S3 dry-run consumes this for its [OK]/[FAIL]
    /// per-line report). `status` is OK iff every check passed.
    struct ValidationReport {
        Status status;
        std::string runtime_version;          ///< Runtime::GetRuntimeVersion()
        int runtime_major = 0;
        std::pair<int, int> supported_opset_range{0, 0};
        std::vector<ModelOpsetInfo> models;
    };

    /// Validate `cfg` against the loaded runtime. Returns OK or the first
    /// failure's Status (version mismatch checked before any model opset).
    ///
    /// TraceContext is nullable at startup (no trace yet); the parameter is
    /// reserved so Phase-2 startup tracing can inject without breaking the
    /// signature (matches the shared `const TraceContext* ctx = nullptr`
    /// convention).
    static Status Validate(
        const ValidationConfig& cfg,
        const observability::TraceContext* ctx = nullptr);

    /// Like Validate(), but returns the full ValidationReport (used by the
    /// --check-onnx dry-run CLI in S3 and by tests that assert per-model detail).
    static ValidationReport ValidateVerbose(
        const ValidationConfig& cfg,
        const observability::TraceContext* ctx = nullptr);

    /// Build a ValidationConfig from a loaded CortrixConfig by collecting the
    /// registered ONNX model consumers (collect_registered_onnx_models).
    ///
    /// Standalone (D3) scope — registers only the consumers whose model_path
    /// already exists in the FROZEN config: the OnnxEmbedder (bge-m3,
    /// `config.embedding.model_path`). The reranker (bge-reranker-v2-m3)
    /// has no model_path field in the current config (it has not landed its
    /// config), so it is NOT collected here — adding it (and the sparse
    /// model) is cross-Feature wiring deferred to D3.5. Empty paths are skipped
    /// (a stub-only deployment registers nothing).
    ///
    /// Defined inline against config.h so this header stays
    /// dependency-light for callers that only need Validate(); see the .cpp.
    static ValidationConfig CollectRegisteredOnnxModels(const CortrixConfig& config);

    /// Read the ONNX opset version from a `.onnx` model file header. ONNX models
    /// are protobuf ModelProto; this reads the `opset_import` field's `version`
    /// for the default (ONNX) domain without linking the full onnx protobuf
    /// library — a minimal, dependency-free wire-format scan (see .cpp).
    ///
    /// Returns the opset on success. On a read/parse failure returns a non-OK
    /// Status (kInvalidArgument with CX_ERR_ONNX_OPSET_INCOMPATIBLE token — a
    /// model we cannot read the opset from cannot be certified compatible).
    static Result<int> ReadModelOpset(const std::string& model_path);
};

}  // namespace cortrix::onnx
