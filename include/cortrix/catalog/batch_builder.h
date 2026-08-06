#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "cortrix/catalog/batch_result.h"

namespace cortrix::catalog {

/// Standard environment for batch responses (meta) — the catalog state
/// stamped onto every BatchMeta so callers don't restate it per call.
struct BatchContext {
    bool    bloom_filter_ready = true;
    int     catalog_version = 0;
    int64_t timestamp_ms = 0;
    std::optional<std::string> node_id;  // 'local' in Phase 1
};

/// Accumulates a partial-success batch and finalizes a BatchResult<T>
/// with the 5 standard meta fields + a coverage_ratio computed from the actual
/// succeeded/failed counts. Keeps the contract (api_version "v1", node_id,
/// catalog_version, bloom_filter_ready, timestamp_ms) in one place.
template <typename T>
class BatchBuilder {
public:
    explicit BatchBuilder(BatchContext ctx) : ctx_(std::move(ctx)) {}

    /// Record a successful item (its payload + id).
    void AddSuccess(const std::string& id, T value) {
        result_.results.push_back(std::move(value));
        result_.meta.succeeded_ids.push_back(id);
    }

    /// Record a failed item (id + CX_ERR_* code + detail).
    void AddFailure(const std::string& id, const std::string& cx_code,
                    const std::string& detail = "") {
        result_.meta.failed.push_back(FailedItem{id, cx_code, detail});
    }

    /// Finalize: stamp meta + compute coverage_ratio = succeeded / total.
    /// An empty batch is fully covered (1.0) by convention.
    BatchResult<T> Build() {
        const size_t ok = result_.meta.succeeded_ids.size();
        const size_t bad = result_.meta.failed.size();
        const size_t total = ok + bad;
        result_.meta.coverage_ratio =
            total == 0 ? 1.0f : static_cast<float>(ok) / static_cast<float>(total);
        result_.meta.bloom_filter_ready = ctx_.bloom_filter_ready;
        result_.meta.catalog_version = ctx_.catalog_version;
        result_.meta.timestamp_ms = ctx_.timestamp_ms;
        result_.meta.node_id = ctx_.node_id;
        // api_version stays "v1" (the struct default; versioning promise).
        return std::move(result_);
    }

private:
    BatchContext ctx_;
    BatchResult<T> result_;
};

}  // namespace cortrix::catalog
