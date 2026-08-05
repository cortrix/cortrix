#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/resource/sqlite_conn.h"
#include "cortrix/store/iindex.h"             // cortrix::store::IIndex
#include "cortrix/store/recover_stores.h"     // IMetadataStore / IBlobStore (recover views)
#include "cortrix/store/write_coordinator.h"  // cortrix::store::WriteCoordinator

namespace cortrix::resource {

/// Runtime resources for one Unit. Owns the per-Unit P-HNSW
/// index handle, the per-NS WriteCoordinator (which itself holds pending.wal),
/// and the per-Unit store.db connection. All three are released together when
/// the UnitResourceBundle is destroyed (UT2).
///
/// Naming: the write resource is the real class `cortrix::store::Write-
/// Coordinator` (a per-NS instance, identified by its data_dir) — there is no
/// PwlHandle wrapper. The field is named `pwl` purely as a semantic label for
/// "the pending-write-log resource", per the spec.
struct UnitResourceBundle {
    std::string unit_id;
    std::string data_dir;   ///< per-Unit dir <data_root>/units/<unit_id> (D3.5 wire④):
                            ///< the NamespaceFacade reads it to site blob/ + memory.db.
    std::unique_ptr<cortrix::store::IIndex> index;             ///< P-HNSW handle
    /// per-Unit store.db connection — ASSEMBLY-TIME ONLY (D-I1.bis): serves the
    /// schema migration, the WC recover views (`meta_recover` borrows it) and the
    /// assembly-time doc crash recovery. Request-path code must NOT borrow it
    /// (the former per-request façade views sharing it across threads segfaulted
    /// under concurrency); the façade opens its own private connection instead.
    std::unique_ptr<SqliteConn> store_db;
    // ── Recover views ──────────────────────────────────────────────────────
    // Narrow, stateless probes the WriteCoordinator borrows (raw ptr) for its
    // three-way Recover consistency check. The pool self-constructs them in
    // LoadOneNamespaceInner (metadata = store_db conn / blob = <unit_dir>/blob),
    // so the resource layer is self-sufficient — the full store/blob façades are
    // NOT required for Recover (D-I4 unbroken). Held here so they outlive `pwl`.
    std::unique_ptr<cortrix::store::IMetadataStore> meta_recover;  ///< borrows store_db conn
    std::unique_ptr<cortrix::store::IBlobStore> blob_recover;      ///< probes <unit_dir>/blob/
    // ★ `pwl` declared LAST → destructs FIRST (members destruct in reverse
    // declaration order). The coordinator borrows index + store_db + the two
    // recover adapters via raw pointers, so it must die before them. §9.3.
    std::unique_ptr<cortrix::store::WriteCoordinator> pwl;     ///< write coordinator (holds pending.wal)

    UnitResourceBundle() = default;
    // Move-only: it owns unique_ptrs. Defaulted moves make NamespaceResourceBundle
    // (which holds a vector of these) move-only too, which is what the pool's
    // bundles_ map needs (insert via std::move).
    UnitResourceBundle(UnitResourceBundle&&) = default;
    UnitResourceBundle& operator=(UnitResourceBundle&&) = default;
    UnitResourceBundle(const UnitResourceBundle&) = delete;
    UnitResourceBundle& operator=(const UnitResourceBundle&) = delete;

    /// Resident memory estimate for this Unit (admission control). Sums the index
    /// footprint (IIndex::GetMemoryFootprintBytes) and the
    /// pending.wal size (WriteCoordinator::GetPwlSizeBytes). A null
    /// resource contributes 0 (write-only / partially-loaded bundles in tests).
    std::size_t MemoryEstimateBytes() const;
};

/// Runtime resources for one Namespace. Phase 1: exactly one
/// Unit (`unit_bundles.size() == 1`, the 1:1 NS↔Unit mapping). Phase 2 (Overflow
/// mode) grows the collection to N Units — the vector
/// shape is the forward-compatible reservation called out in topic 1.
struct NamespaceResourceBundle {
    std::string namespace_id;
    std::vector<UnitResourceBundle> unit_bundles;

    NamespaceResourceBundle() = default;
    NamespaceResourceBundle(NamespaceResourceBundle&&) = default;
    NamespaceResourceBundle& operator=(NamespaceResourceBundle&&) = default;
    NamespaceResourceBundle(const NamespaceResourceBundle&) = delete;
    NamespaceResourceBundle& operator=(const NamespaceResourceBundle&) = delete;

    /// The Phase-1 sole Unit. Precondition: unit_bundles is non-empty (a loaded
    /// bundle always has its one Unit). Phase 2 callers iterate unit_bundles.
    UnitResourceBundle& primary() { return unit_bundles.front(); }
    const UnitResourceBundle& primary() const { return unit_bundles.front(); }

    /// Total resident memory across all Units (used by the budget check).
    std::size_t TotalMemoryEstimateBytes() const;
};

}  // namespace cortrix::resource
