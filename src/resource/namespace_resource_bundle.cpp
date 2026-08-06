#include "cortrix/resource/namespace_resource_bundle.h"

namespace cortrix::resource {

std::size_t UnitResourceBundle::MemoryEstimateBytes() const {
    std::size_t total = 0;
    // P-HNSW resident footprint (pool hook, IIndex::GetMemoryFootprintBytes).
    if (index) total += index->GetMemoryFootprintBytes();
    // pending.wal size (pool hook, WriteCoordinator::GetPwlSizeBytes). The
    // store.db connection's own SQLite cache is bounded by the PRAGMA cache_size
    // (/Unit) and is not separately accounted here — the index dominates.
    if (pwl) total += pwl->GetPwlSizeBytes();
    return total;
}

std::size_t NamespaceResourceBundle::TotalMemoryEstimateBytes() const {
    std::size_t total = 0;
    for (const auto& unit : unit_bundles) {
        total += unit.MemoryEstimateBytes();
    }
    return total;
}

}  // namespace cortrix::resource
