#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "cortrix/common/status.h"

// TraceContext is forward-declared (referenced by pointer only), mirroring
// cortrix/store/iindex.h, so this minimal store contract stays decoupled from
// the observability build (OBSERVABILITY_SPEC §5.3).
namespace cortrix::observability {
struct TraceContext;
}

namespace cortrix::store {

/// Minimal vector-store contract held by F25 WriteCoordinator (D1-Storage Q1/Q4/Q5,
/// 2026-04-19; ARCHITECTURE.md §5). This is the **6-method subset** of the index
/// surface that the cross-store write transaction actually needs — AddPoint +
/// MarkDelete for the write/rollback paths, Search for reads, Exists for F25's
/// three-way Recover consistency check (Q1-C), and Snapshot/Recover for lifecycle.
///
/// SoT note (ARCHITECTURE.md D-07 / V13): the rich vector-index SoT is F01's
/// IIndex (cortrix/store/iindex.h, 10 methods incl. AddPoints/Shutdown/GetStats/
/// GetMemoryFootprintBytes). PHnsw implements all of IIndex and therefore
/// satisfies this subset too; F25 holds only an `IVectorStore*` so it depends on
/// the small surface, not the full index API.
///
/// This interface is intentionally NOT derived from IIndex: its Search has no
/// ef_search knob (F25 uses index defaults) and its Exists/Snapshot/Recover carry
/// a TraceContext, so the two surfaces differ. PHnsw inherits both and overrides
/// each independently.
class IVectorStore {
public:
    virtual ~IVectorStore() = default;

    virtual Status AddPoint(const float* vec, uint64_t block_id,
                            const observability::TraceContext* ctx = nullptr) = 0;
    /// Idempotent: a missing block_id returns Ok (F25 Q6 + V5 #9), so a
    /// transaction rollback can replay the cleanup safely.
    virtual Status MarkDelete(uint64_t block_id,
                              const observability::TraceContext* ctx = nullptr) = 0;
    virtual std::vector<std::pair<uint64_t, float>> Search(
        const float* query, int top_k,
        const observability::TraceContext* ctx = nullptr) = 0;
    /// Membership check — required by F25 Recover three-way consistency (Q1-C):
    /// F01.Exists() + SQLite.BlockExists() + Blob.BlobExists() all present =>
    /// the txn is inferred COMMITTED.
    virtual bool Exists(uint64_t block_id,
                        const observability::TraceContext* ctx = nullptr) = 0;
    virtual Status Snapshot(const observability::TraceContext* ctx = nullptr) = 0;
    virtual Status Recover(const observability::TraceContext* ctx = nullptr) = 0;
};

}  // namespace cortrix::store
