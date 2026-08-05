#pragma once

namespace cortrix::ml {

/// Process-wide ONNX Runtime environment singleton (the lifecycle SoT).
///
/// Background: OnnxReranker, BGE-M3 sparse and OnnxEmbedder
/// all need an Ort::Env. Creating one per component duplicates the global runtime
/// state. This singleton owns the single Ort::Env; consumers borrow it.
///
/// Thread-safety: lazily initialized exactly once via std::call_once on first
/// EnvHandle()/Init() call ("std::call_once guarantees one-time initialization").
///
/// Standalone (D3): no real model needed to construct the Env — Ort::Env is just
/// the logging/threadpool context, so this is fully testable in-process. The
/// concrete Ort::Env type stays out of this header (opaque void*), mirroring the
/// in-tree OnnxEmbedder pattern, so non-ONNX translation units can include it.
class OrtEnvSingleton {
public:
    /// Ensure the Env is created (idempotent; call_once). No-op + returns false
    /// when the binary was built without ONNX Runtime (CORTRIX_HAS_ONNXRUNTIME
    /// undefined), so callers can fail-fast cleanly.
    static bool Init();

    /// True once the Env has been created (test/health hook).
    static bool Initialized();

    /// Opaque handle to the shared Ort::Env* (nullptr if ONNX is not compiled in
    /// or Init() has not run). Callers that have the ONNX headers cast it back to
    /// Ort::Env*. Triggers Init() on first use.
    static void* EnvHandle();

private:
    OrtEnvSingleton() = default;
};

}  // namespace cortrix::ml
