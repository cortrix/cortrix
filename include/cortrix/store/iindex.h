#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "cortrix/common/status.h"

// TraceContext is defined by the observability scaffolding
// (the observability spec, cortrix/observability/trace_context.h). It is only
// referenced by pointer here, so a forward declaration keeps IIndex decoupled
// from the observability build.
namespace cortrix::observability {
struct TraceContext;
}

namespace cortrix::store {

/// Snapshot of index runtime state (IIndex::GetStats). Backends fill what they
/// track; unsupported counters stay at their defaults.
struct IndexStats {
    uint64_t vector_count = 0;          ///< live vectors (excludes MarkDelete'd)
    uint64_t deleted_count = 0;         ///< logically deleted, not yet reclaimed
    std::size_t memory_footprint_bytes = 0;
    int dim = 0;
};

/// Vector index contract (evolved from the MVP CortrixVectorIndex to the rich
/// API). Implemented by P-HNSW (and Phase 2 BruteForce / QuantizedHnsw);
/// consumed via IIndexFactory by the catalog and NamespacePool.
///
/// Error model: write/lifecycle ops return Status; Search returns its hits
/// directly; MarkDelete is idempotent (missing block_id returns Ok
/// + V5 #9). Every mutating/search op accepts an optional TraceContext for the
/// observability span chain (OBS_SPEC).
class IIndex {
public:
    virtual ~IIndex() = default;

    // --- Writes ---
    virtual Status AddPoint(const float* vec, uint64_t block_id,
                            const observability::TraceContext* ctx = nullptr) = 0;
    virtual Status AddPoints(const std::vector<std::pair<const float*, uint64_t>>& points,
                             const observability::TraceContext* ctx = nullptr) = 0;
    /// Idempotent: a missing block_id returns Ok so rollback replays are safe.
    virtual Status MarkDelete(uint64_t block_id,
                              const observability::TraceContext* ctx = nullptr) = 0;

    // --- Search ---
    virtual std::vector<std::pair<uint64_t, float>> Search(
        const float* query, int top_k, int ef_search = -1,
        const observability::TraceContext* ctx = nullptr) = 0;

    /// Membership check used by Recover three-way consistency.
    virtual bool Exists(uint64_t block_id) = 0;

    // --- Persistence / lifecycle ---
    virtual Status Snapshot() = 0;
    virtual Status Recover() = 0;
    virtual Status Shutdown() = 0;
    virtual IndexStats GetStats() = 0;

    /// Resident memory estimate (bytes) for admission control.
    virtual std::size_t GetMemoryFootprintBytes() const = 0;
};

}  // namespace cortrix::store
