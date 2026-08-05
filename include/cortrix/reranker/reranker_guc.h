#pragma once
#include <string>

#include "cortrix/common/i_global_config.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/reranker/reranker_config.h"

namespace cortrix::reranker {

/// Reranker GUC keys + range validation (F02 §2.4 GUC table SoT in code).
///
/// Standalone (D3): defines the GUC names + [min,max] ranges and provides pure
/// validation + a load-from-IGlobalConfig path. Registering these with the live
/// PostgreSQL GUC machinery (DefineCustomIntVariable etc.) is the pgcortrix /
/// server integration → D3.5; here the validated values flow into RerankerConfig
/// for the OnnxReranker + RerankerThreadPool.
///
/// Range-violation policy: REJECT (return Status error), not silent clamp — a GUC
/// set outside its documented range is an operator error surfaced at config load
/// (topic S2.2 DoD "reject out-of-range").
namespace guc {
// Keys. Resident-resource GUCs are global (not NS-overridable).
inline constexpr const char* kQueueSize       = "reranker.queue_size";
inline constexpr const char* kTaskTimeoutMs   = "reranker.task_timeout_ms";
inline constexpr const char* kWorkers         = "reranker.workers";
inline constexpr const char* kMaxSeqLength    = "reranker.max_seq_length";
inline constexpr const char* kCircuitThreshold = "reranker.circuit_breaker.threshold";
inline constexpr const char* kCircuitCooldown = "reranker.circuit_breaker.cooldown_sec";

// spc.chunk_size (F02 §2.4 "V5-B2-05 GUC registration backfill") — owned by SPC but read here
// for the §3.5.1 startup compat check (spc.chunk_size ≤ reranker.max_seq_length).
// Its [min,max] mirror reranker.max_seq_length (both bound by the model's token
// limit); the default 512 matches recursive_chunker's chunk_size default.
inline constexpr const char* kSpcChunkSize    = "spc.chunk_size";
inline constexpr int kSpcChunkSizeDefault = 512;

// Ranges. Inclusive [min, max].
inline constexpr int kQueueSizeMin = 50,      kQueueSizeMax = 2000;
inline constexpr int kTaskTimeoutMin = 1000,  kTaskTimeoutMax = 30000;
inline constexpr int kWorkersMin = 1,         kWorkersMax = 16;
inline constexpr int kMaxSeqLenMin = 128,     kMaxSeqLenMax = 8192;
inline constexpr int kSpcChunkSizeMin = 128,  kSpcChunkSizeMax = 8192;
}  // namespace guc

class RerankerGuc {
public:
    /// Validate every ranged field of `config` against its §2.4 range. Returns OK
    /// or the first out-of-range field's Status (InvalidArgument with the field
    /// name + value + range in the message). Validates queue_size + task_timeout_ms
    /// + workers + max_seq_length + circuit-breaker bounds.
    static Status ValidateConfig(const RerankerConfig& config);

    /// Validate a single value against [min, max]; OK or InvalidArgument naming
    /// `key`, the value and the range. Exposed for per-GUC set-time checks.
    static Status ValidateRange(const std::string& key, int value, int min, int max);

    /// Read reranker.* keys from `cfg` into `out` (keeping the struct default when
    /// a key is absent), then ValidateConfig(*out). Returns OK or the validation
    /// error. (Live PostgreSQL GUC registration = D3.5; this is the standalone
    /// IGlobalConfig path.)
    static Status LoadFromGlobalConfig(const IGlobalConfig& cfg, RerankerConfig* out);

    /// Read `spc.chunk_size` from `cfg` (F02 §2.4 V5-B2-05). Returns the configured
    /// value, or kSpcChunkSizeDefault (512) when the key is absent. Validates it
    /// against [kSpcChunkSizeMin, kSpcChunkSizeMax]; an out-of-range value is
    /// InvalidArgument (consistent with the reject-not-clamp policy). Used by the
    /// startup compat check (S3.2) that pairs it with reranker.max_seq_length.
    static Result<int> LoadSpcChunkSize(const IGlobalConfig& cfg);
};

}  // namespace cortrix::reranker
