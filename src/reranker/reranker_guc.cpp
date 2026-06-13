#include "cortrix/reranker/reranker_guc.h"

#include <string>

namespace cortrix::reranker {

Status RerankerGuc::ValidateRange(const std::string& key, int value, int min, int max) {
    if (value < min || value > max) {
        return Status::InvalidArgument(
            key + "=" + std::to_string(value) + " out of range [" +
            std::to_string(min) + ", " + std::to_string(max) + "]");
    }
    return Status::Ok();
}

Status RerankerGuc::ValidateConfig(const RerankerConfig& config) {
    Status s;
    s = ValidateRange(guc::kQueueSize, config.queue_size, guc::kQueueSizeMin, guc::kQueueSizeMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kTaskTimeoutMs, config.task_timeout_ms,
                      guc::kTaskTimeoutMin, guc::kTaskTimeoutMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kWorkers, config.workers, guc::kWorkersMin, guc::kWorkersMax);
    if (!s.ok()) return s;
    s = ValidateRange(guc::kMaxSeqLength, config.max_seq_length,
                      guc::kMaxSeqLenMin, guc::kMaxSeqLenMax);
    if (!s.ok()) return s;
    // Circuit-breaker bounds are sanity-checked (must be positive); §2.4 lists no
    // hard upper bound, so only reject non-positive values.
    if (config.circuit_breaker_threshold <= 0) {
        return Status::InvalidArgument(std::string(guc::kCircuitThreshold) +
                                       " must be > 0");
    }
    if (config.circuit_breaker_cooldown_sec <= 0) {
        return Status::InvalidArgument(std::string(guc::kCircuitCooldown) +
                                       " must be > 0");
    }
    return Status::Ok();
}

Status RerankerGuc::LoadFromGlobalConfig(const IGlobalConfig& cfg, RerankerConfig* out) {
    if (out == nullptr) return Status::InvalidArgument("null RerankerConfig out");

    // Each key is optional: a present value overrides the struct default; an
    // absent key (GetInt not ok) leaves the default in place.
    auto load_int = [&cfg](const char* key, int* field) {
        Result<int> r = cfg.GetInt(key);
        if (r.ok()) *field = r.value();
    };
    auto load_bool = [&cfg](const char* key, bool* field) {
        Result<bool> r = cfg.GetBool(key);
        if (r.ok()) *field = r.value();
    };

    load_int(guc::kQueueSize, &out->queue_size);
    load_int(guc::kTaskTimeoutMs, &out->task_timeout_ms);
    load_int(guc::kWorkers, &out->workers);
    load_int(guc::kMaxSeqLength, &out->max_seq_length);
    load_int(guc::kCircuitThreshold, &out->circuit_breaker_threshold);
    load_int(guc::kCircuitCooldown, &out->circuit_breaker_cooldown_sec);
    load_bool("reranker.circuit_breaker.enabled", &out->circuit_breaker_enabled);

    return ValidateConfig(*out);
}

Result<int> RerankerGuc::LoadSpcChunkSize(const IGlobalConfig& cfg) {
    int value = guc::kSpcChunkSizeDefault;  // 512 when absent
    Result<int> r = cfg.GetInt(guc::kSpcChunkSize);
    if (r.ok()) value = r.value();
    Status s = ValidateRange(guc::kSpcChunkSize, value,
                             guc::kSpcChunkSizeMin, guc::kSpcChunkSizeMax);
    if (!s.ok()) return s;
    return value;
}

}  // namespace cortrix::reranker
