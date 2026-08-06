#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace cortrix::onnx {

/// Thin wrapper over the ONNX Runtime C API (thin, not fat).
/// Provides version introspection + a home for the few incremental shims we may
/// need for known breaking changes across supported runtime versions. There is
/// deliberately NO general abstraction of the Ort API: consumers (OnnxEmbedder,
/// the reranker) keep calling Ort directly; this class only owns the
/// version/opset facts the StartupValidator needs.
///
/// Phase 1: static methods (one runtime instance per Cortrix binary, build-time
/// locked per=A). A future multi-runtime mode would require a breaking change
/// to an instance-based API in Phase 2.
class Runtime {
 public:
    /// The ONNX Runtime version actually loaded into this process, e.g.
    /// "1.17.1". Read from libonnxruntime via OrtGetApiBase()->GetVersionString().
    /// When the binary is built without ONNX Runtime (CORTRIX_USE_ONNX=OFF),
    /// returns the empty string — callers treat that as "no runtime present".
    static std::string GetRuntimeVersion();

    /// The major-version component of GetRuntimeVersion() (e.g. "1.17.1" -> 1).
    /// Returns 0 when no runtime is present / the version string is unparseable.
    static int GetRuntimeMajorVersion();

    /// The supported ONNX opset range [min_opset, max_opset] for the loaded
    /// runtime. ONNX Runtime does not expose this programmatically, so the range
    /// is derived from the loaded runtime major/minor via a small static table
    /// (the public ORT release notes' opset support matrix). Returns {0, 0} when
    /// no runtime is present.
    static std::pair<int32_t, int32_t> GetSupportedOpsetRange();

    /// The ONNX Runtime ABI major version this binary was COMPILED against,
    /// from the CMake-locked CORTRIX_ONNXRT_EXPECTED_MAJOR. Compared
    /// with GetRuntimeMajorVersion() at startup to detect an ABI mismatch after
    /// a `.so` swap. Returned as a string to match the structured_data
    /// "expected_major_version" string-typed contract.
    static std::string GetCompiledMajorVersion();

    /// True when this binary was built with ONNX Runtime linked
    /// (CORTRIX_USE_ONNX=ON → CORTRIX_HAS_ONNXRUNTIME defined). When false, the
    /// version/opset getters return their "absent" sentinels and the
    /// StartupValidator treats the runtime as unavailable rather than mismatched.
    static bool IsRuntimeLinked();
};

}  // namespace cortrix::onnx
