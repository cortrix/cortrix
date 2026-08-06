#pragma once
#include <memory>
#include <string>

#include "cortrix/common/status.h"
#include "cortrix/memory/memory_store.h"          // MemoryStore
#include "cortrix/resource/namespace_pool.h"      // INamespacePool, NamespaceResourceBundle (+IIndex/WC via bundle)
#include "cortrix/store/cortrix_blob_local.h"     // CortrixBlobLocal (+ CortrixBlobStore base)
#include "cortrix/store/cortrix_store_sqlite.h"   // CortrixStoreSqlite (D-I1 external-conn) (+ CortrixStore base)

namespace cortrix::resource {

/// integration wire④ — the thin façade over one acquired Namespace (NAMESPACEPOOL_NS_INTEGRATION
///). Phase-1 access model (= authoritative): a consumer constructs a
/// NamespaceFacade per request, calls Acquire() (= Pool.Acquire), uses the
/// five windows, and lets the destructor Release() (D-I5(a) per-request).
///
/// Layering: the Pool owns the "heavy" resources (P-HNSW index +
/// WriteCoordinator/pending.wal, which need admission); the façade owns the
/// "light" resources it builds itself (D-I4) — the blob file dir + the independent
/// memory.db — plus its own PRIVATE store.db connection (D-I1.bis: per-facade
/// WAL connection, thread isolation via object lifetime; the former shared
/// bundle-conn view segfaulted under concurrency and was overturned).
///
/// This is NOT the MVP CortrixNamespace copied; it is strictly a wrapper on top of
/// Acquire (the façade may only be a thin wrapper above Acquire).
class NamespaceFacade {
public:
    /// Bind to a pool + namespace. Does NOT acquire yet — call Acquire() first.
    NamespaceFacade(INamespacePool& pool, std::string namespace_id);

    /// Releases the acquired bundle (if any) — RAII pairing of Acquire (D-I5a).
    ~NamespaceFacade();

    NamespaceFacade(const NamespaceFacade&) = delete;
    NamespaceFacade& operator=(const NamespaceFacade&) = delete;

    /// Acquire the bundle from the pool + build the self-held windows. Returns the
    /// pool's Status (CX_ERR_NS_* token) on failure, leaving the façade unusable
    /// (the destructor is then a no-op). Call exactly once before any window.
    Status Acquire();

    bool acquired() const { return acquired_; }
    const std::string& namespace_id() const { return namespace_id_; }

    // ── Five windows. Precondition: Acquire() returned Ok. ──────────────

    /// Bundle's P-HNSW index (IIndex) — the heavy resource (forwarded).
    cortrix::store::IIndex& vec_index();
    /// Bundle's per-NS WriteCoordinator (the `pwl` resource) (forwarded).
    cortrix::store::WriteCoordinator& write_coordinator();
    /// CortrixStore over this façade's PRIVATE store.db connection (D-I1.bis).
    cortrix::CortrixStore& store();
    /// Self-held blob store rooted at <unit_dir>/blob (D-I4 light resource).
    cortrix::CortrixBlobStore& blob();
    /// Self-held memory store on its own <unit_dir>/memory.db (D-I4 light resource).
    cortrix::MemoryStore& memory();

private:
    INamespacePool& pool_;
    std::string namespace_id_;
    NamespaceResourceBundle* bundle_ = nullptr;   // borrowed from the pool (not owned)
    bool acquired_ = false;

    // Self-held light resources (D-I4). Torn down (reverse) before Release.
    std::unique_ptr<cortrix::CortrixStoreSqlite> store_;   // private conn (D-I1.bis)
    std::unique_ptr<cortrix::CortrixBlobLocal> blob_;      // <unit_dir>/blob
    std::unique_ptr<cortrix::MemoryStore> memory_;         // <unit_dir>/memory.db
};

}  // namespace cortrix::resource
