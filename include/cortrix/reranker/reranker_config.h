#pragma once
#include <string>

namespace cortrix::reranker {

/// Full RerankerConfig. Default values + ranges mirror the GUC table
///; range enforcement lives in the GUC-registration path (S2.2 / S3.1),
/// the struct itself is a plain value carrier.
struct RerankerConfig {
    // --- Model paths ---
    std::string model_path;       ///< bge-reranker-v2-m3/model.onnx
    std::string tokenizer_path;   ///< bge-reranker-v2-m3/tokenizer.json

    // --- EP selection ---
    // Keep this legacy field in its original aggregate position. New callers
    // should use execution_provider; conflicting values fail initialization.
    enum class UseCoreML { kAuto, kForceTrue, kForceFalse };
    UseCoreML use_coreml = UseCoreML::kAuto;
    int intra_threads = 0;        ///< 0 = ONNX auto

    // --- Thread pool (topic 1 / 3.3) ---
    int workers = 4;
    int queue_size = 200;         ///< range 50-2000
    int task_timeout_ms = 5000;   ///< range 1000-30000

    // --- Circuit breaker (topic 1.3 / 3.4) ---
    bool circuit_breaker_enabled = true;
    int circuit_breaker_threshold = 10;
    int circuit_breaker_cooldown_sec = 30;

    // --- Input preprocessing (topic 3.5) ---
    int max_seq_length = 512;     ///< range 128-8192

    // --- top_N (E2 ruling) ---
    int candidate_multiplier = 3; ///< top_N = min(top_k × N, max_candidates)
    int max_candidates = 50;

    // Canonical EP selection (auto, cpu, coreml, cuda). Appended to preserve
    // source compatibility for existing aggregate initialization.
    std::string execution_provider = "auto";
};

}  // namespace cortrix::reranker
