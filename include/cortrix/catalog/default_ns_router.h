#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_types.h"
#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/ttl_lru_cache.h"

typedef struct sqlite3 sqlite3;

namespace cortrix::resource {
// Forward declaration ONLY — F05 (INamespacePool) is Layer 1, built after F12
// (Layer 0). F12's HEADER MUST compile standalone, so we never #include an F05
// header here; the dependency direction is strictly F05 → F12. The .cpp DOES
// include namespace_pool.h to make the F13 admission calls (it is compiled in
// the full build where F05 exists). The Phase-1 default (f05_pool_ == nullptr)
// skips admission.
class INamespacePool;
}  // namespace cortrix::resource

namespace cortrix::catalog {

/// catalog.db-backed INSRouter (F12 §3.1 impl, Story S2.2). Borrows the sqlite3
/// handle owned by CatalogDb (does not open/close it). Reads/writes the
/// namespaces / units / ns_units tables and fronts them with the §6.3 caches:
/// NS metadata (10K / 60s) + Unit descriptor (1K / 60s).
///
/// Error model (F-FREEZE-1): value methods return Result<T> whose error path is a
/// Status carrying the CX_ERR_* token (CatalogStatus); lifecycle methods return
/// Status. The full Agent-friendly body is built at the boundary.
///
/// Thread-safety: a single mutex guards the sqlite handle + both caches (SQLite
/// connection is not shared concurrently; catalog ops are short).
class DefaultINSRouter : public INSRouter {
public:
    /// Construct over an already-opened catalog.db handle (from CatalogDb::db()).
    /// `f05_pool` is the optional Layer-1 admission hook (nullptr in Phase 1 /
    /// F12-standalone → admission is skipped). Cache sizes/TTL default to §6.3.
    explicit DefaultINSRouter(sqlite3* db, resource::INamespacePool* f05_pool = nullptr);

    /// F13: late-bind the F05 admission hook after construction. Breaks the
    /// pool↔router ctor cycle (the pool ctor needs the router to load the catalog;
    /// the router needs the pool to AdmitCreate on CreateNamespace). main calls this
    /// once, after the pool is built + StartupLoadAll. nullptr = admission skipped.
    void SetPool(resource::INamespacePool* pool) { f05_pool_ = pool; }

    Result<std::vector<UnitDescriptor>> LookupUnits(
        const std::string& namespace_id) const override;
    Result<UnitDescriptor> GetActiveUnit(
        const std::string& namespace_id) const override;
    Result<NSMetadata> GetNamespace(
        const std::string& namespace_id) const override;
    Result<BatchResult<std::string>> ListNamespaces(
        const ListNamespacesOptions& opts = {}) const override;
    Status CreateNamespace(const NSMetadata& metadata) override;
    Status DeleteNamespace(const std::string& namespace_id) override;

private:
    // Read one units row by unit_id (no cache); caller holds mu_.
    Result<UnitDescriptor> ReadUnitLocked(const std::string& unit_id) const;
    // Read one namespaces row by ns_id (no cache); caller holds mu_.
    Result<NSMetadata> ReadNamespaceLocked(const std::string& ns_id) const;
    // The single Unit mapped to ns_id (Phase 1 1:1); caller holds mu_.
    Result<std::string> ActiveUnitIdLocked(const std::string& ns_id) const;
    // The durable catalog write of CreateNamespace (INSERT ns + Unit + ns_units in
    // one txn). Extracted so the F05 admission/rollback wraps it; takes
    // mu_ itself.
    Status InsertNamespaceCatalog(const NSMetadata& metadata);
    // Compensation for a failed AdmitCreate: physically remove the
    // rows InsertNamespaceCatalog just wrote (the NS never became a successful
    // create). NOT the user-facing soft delete — that would block a same-name
    // retry on the PK and ListNamespaces does not filter 'deleted' rows. Takes mu_.
    void RemoveNamespaceCatalogRows(const std::string& ns_id);

    sqlite3* db_;                    // borrowed, not owned
    resource::INamespacePool* f05_pool_;  // optional Layer-1 hook (may be null)
    mutable std::mutex mu_;
    mutable TtlLruCache<std::string, NSMetadata> ns_cache_;
    mutable TtlLruCache<std::string, UnitDescriptor> unit_cache_;
};

}  // namespace cortrix::catalog
