#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cortrix/catalog/i_ns_router.h"
#include "cortrix/catalog/i_unit_router.h"
#include "cortrix/common/result.h"
#include "cortrix/common/status.h"
#include "cortrix/resource/f05_config.h"
#include "cortrix/resource/namespace_resource_bundle.h"
#include "cortrix/resource/pool_error.h"
#include "cortrix/store/iindex_factory.h"
#include "cortrix/store/write_coordinator.h"

namespace cortrix::resource {

// ── Stats / report value types (F05 §3.2) ───────────────────────────────────

/// Rejected-create counters, nested to mirror the admin API JSON 1:1
/// (`rejected_creates_total.{ns_count_exceeded, memory_exceeded}`, §8.2) so the
/// C++ → JSON serialization needs no field-name mapping (§3.2 v1.0.1 m6).
struct RejectedCreatesTotal {
    size_t ns_count_exceeded = 0;
    size_t memory_exceeded = 0;
};

/// A-class fields (topic 6 default-exposed): admin API + Prometheus metric (§9).
struct PoolStats {
    size_t pool_size_current = 0;
    size_t pool_size_max = 0;
    size_t memory_budget_used_bytes = 0;
    size_t memory_budget_limit_bytes = 0;   ///< 0 = budget disabled
    RejectedCreatesTotal rejected_creates_total;
    size_t startup_load_failures = 0;
};

/// One admission rejection, kept in the recent-rejections ring buffer (C class,
/// §3.2 / §8.2 explain endpoint). `reason` is "ns_count_exceeded" or
/// "memory_exceeded" — the enumerable values from RejectedCreatesTotal (§10.1).
struct RejectionEvent {
    int64_t timestamp_ms = 0;
    std::string namespace_id;
    std::string reason;
    size_t budget_used = 0;
    size_t budget_limit = 0;
    size_t estimated_size = 0;
};

/// Outcome of StartupLoadAll (F05 §6, S2). total = loaded + failed.
struct StartupReport {
    size_t total_namespaces = 0;
    size_t loaded_successfully = 0;
    size_t failed = 0;
    std::vector<std::string> failed_namespaces;
    int64_t total_duration_ms = 0;
};

/// C-class debug snapshot (topic 6, only `/api/v1/system/pool/explain`, §8.2).
struct ExplainState {
    std::vector<std::string> active_namespaces;
    size_t memory_estimate_bytes = 0;
    std::vector<std::string> startup_failed_namespaces;
    std::vector<RejectionEvent> recent_rejections;  ///< most recent N=50
};

/// Factory that constructs a per-NS F25 WriteCoordinator (D3.5 C1 / §9.3). The
/// pool calls it during load with the NS's data_dir + tuning AND the three stores
/// the coordinator's Recover() needs — the pool resolves these in
/// LoadOneNamespaceInner (vector = the Unit's index cast to IVectorStore; metadata
/// + blob = the two narrow recover adapters the pool self-constructs, §9.2). The
/// factory news the WriteCoordinator over them and Init()s it; injecting it (vs
/// newing directly) keeps the pool testable with a stub coordinator. Returns a
/// Result so a construction/Init failure maps to CX_ERR_NS_LOAD_FAILED.
using WriteCoordinatorFactory =
    std::function<Result<std::unique_ptr<cortrix::store::WriteCoordinator>>(
        const std::string& data_dir,
        const cortrix::store::WriteCoordinatorConfig& config,
        cortrix::store::IVectorStore* vector_store,
        cortrix::store::IMetadataStore* metadata_store,
        cortrix::store::IBlobStore* blob_store)>;

// ── INamespacePool (F05 §3.2) ────────────────────────────────────────────────

/// NS resource pool contract (F05 §2.1). Manages per-NS runtime resource bundles
/// (index / WriteCoordinator / store.db) with NS-count + memory-budget admission
/// control. Phase 1: all-hot, no eviction (TD-F05-EVICTION is Phase 2).
///
/// Error model (CODING_CONVENTIONS §3 / F-FREEZE-1): the spec's
/// `Result<…, PoolError>` is read as Result<T> (value path) or plain Status
/// (void path). Every error Status carries a CX_ERR_NS_* token (PoolStatus());
/// the API/SDK boundary re-inflates the Agent-friendly body via MakePoolError().
class INamespacePool {
public:
    virtual ~INamespacePool() = default;

    // —— Create path (admission control) ——
    /// Called by F12.INSRouter.CreateNamespace before the catalog INSERT (F12-N
    /// reverse hook, §5.1). Checks the NS-count gate, then the memory-budget gate
    /// (if enabled), then loads the new NS into the pool. On success the NS is
    /// resident and Acquire will hit. Errors (as Status w/ CX_ERR_NS_* token):
    /// CX_ERR_NS_QUOTA_EXCEEDED / CX_ERR_NS_RESOURCE_BUDGET_EXCEEDED /
    /// CX_ERR_NS_LOAD_FAILED. Wiring this into the real INSRouter is D3.5.
    virtual Status AdmitCreate(const std::string& namespace_id,
                               size_t estimated_size_bytes) = 0;

    // —— Access path ——
    /// Borrow a resident NS's bundle (Phase 1 always hot → O(1) hit). The pointer
    /// is owned by the pool and valid until EvictForDelete (Phase 1 never evicts
    /// otherwise). Error: CX_ERR_NS_NOT_FOUND (never created / load failed).
    virtual Result<NamespaceResourceBundle*> Acquire(
        const std::string& namespace_id) = 0;

    /// Phase 1 no-op (the refcount/drain seam reserved for Phase 2 eviction,
    /// §14 TD-F05-EVICTION). Present so callers (F25 BeginWrite) pair Acquire.
    virtual void Release(const std::string& namespace_id) = 0;

    // —— Startup path ——
    /// Eager-load every NS in the catalog at startup, 8 workers concurrent (§6).
    /// Partial failure is reported in the StartupReport (failed_namespaces) rather
    /// than aborting startup; failed NS are retried via ReloadNamespace.
    virtual Result<StartupReport> StartupLoadAll() = 0;

    /// Admin retry of a NS that failed to load (post-startup, §8.3.3). Status
    /// error: CX_ERR_NS_LOAD_FAILED if it still cannot be loaded.
    virtual Status ReloadNamespace(const std::string& namespace_id) = 0;

    // —— Unload path ——
    /// Drop a NS's resources from the pool. Called only by F12.DeleteNamespace
    /// (NS soft-delete, §13.1) and by the F12 admission rollback (§5.2) to undo a
    /// pool add when the catalog INSERT fails. Idempotent: absent NS returns Ok.
    virtual Status EvictForDelete(const std::string& namespace_id) = 0;

    // —— State queries ——
    virtual PoolStats GetPoolStats() const = 0;
    virtual ExplainState GetExplainState() const = 0;
};

// ── DefaultNamespacePool (F05 §3.3) ──────────────────────────────────────────

/// Phase-1 in-process implementation of INamespacePool. Resources are held in a
/// flat `bundles_` map under a shared_mutex (concurrent Acquire readers; exclusive
/// admit/load/evict writers). No eviction in Phase 1 (topic 4 deleted).
class DefaultNamespacePool : public INamespacePool {
public:
    /// @param index_factory        borrowed F01 factory (Open per Unit)
    /// @param write_coord_factory  per-NS F25 coordinator factory (v1.0.2 §3.3)
    /// @param ns_router            borrowed F12 NS router (catalog NS list / lookup)
    /// @param unit_router          borrowed F12 Unit router (active Unit descriptor)
    /// @param config               global F05 config (§4.1; standalone passes it
    ///                             directly — the IGlobalConfig getter is D3.5)
    DefaultNamespacePool(cortrix::store::IIndexFactory* index_factory,
                         WriteCoordinatorFactory write_coord_factory,
                         cortrix::catalog::INSRouter* ns_router,
                         cortrix::catalog::IUnitRouter* unit_router,
                         F05Config config);

    Status AdmitCreate(const std::string& namespace_id,
                       size_t estimated_size_bytes) override;
    Result<NamespaceResourceBundle*> Acquire(const std::string& namespace_id) override;
    void Release(const std::string& namespace_id) override;
    Result<StartupReport> StartupLoadAll() override;
    Status ReloadNamespace(const std::string& namespace_id) override;
    Status EvictForDelete(const std::string& namespace_id) override;
    PoolStats GetPoolStats() const override;
    ExplainState GetExplainState() const override;

private:
    // Sum of every resident bundle's memory estimate. Caller holds pool_mutex_.
    size_t CurrentMemoryUsedLocked() const;

    // Snapshot the resident pool under a shared lock and push size +
    // memory_budget_used to the §10.1 ns_pool gauges. Caller must NOT already hold
    // pool_mutex_ (takes a shared lock itself).
    void RefreshPoolGauges() const;

    // Append one rejection to the ring buffer (cap 50), under rejection_log_mutex_.
    void RecordRejection(const std::string& namespace_id, const char* reason,
                         size_t budget_used, size_t budget_limit,
                         size_t estimated_size);

    // Load one NS's resources into a bundle (no pool insert). Used by AdmitCreate,
    // ReloadNamespace, and StartupLoadAll. Returns the built bundle or a
    // CX_ERR_NS_LOAD_FAILED Status. Does not take pool_mutex_.
    Result<NamespaceResourceBundle> LoadOneNamespaceInner(const std::string& ns_id);

    // Timeout-guarded wrapper around LoadOneNamespaceInner (§6.3 C1 fix). Runs the
    // inner load on a std::async task and waits up to load_timeout_ms_per_ns; a
    // timeout is reported as CX_ERR_NS_LOAD_FAILED without force-killing the
    // future (avoids partial-state corruption — the orphaned task finishes in the
    // background; this NS is simply not admitted and can be ReloadNamespace'd).
    Result<NamespaceResourceBundle> LoadOneNamespace(const std::string& ns_id);

    mutable std::shared_mutex pool_mutex_;
    std::unordered_map<std::string, NamespaceResourceBundle> bundles_;

    cortrix::store::IIndexFactory* index_factory_;
    WriteCoordinatorFactory write_coord_factory_;
    cortrix::catalog::INSRouter* ns_router_;
    // Held per the F05 §3.3 contract / topic 8 (F12 coordination boundary). The standalone load
    // path resolves the Unit through INSRouter::GetActiveUnit (the L0 catalog
    // contract returns the full UnitDescriptor directly, so a separate
    // IUnitRouter::GetUnit call is unnecessary here); this stays wired for the
    // D3.5 F12-N hook + Phase-2 multi-Unit (TD-F05-MULTI-UNIT) lookups.
    [[maybe_unused]] cortrix::catalog::IUnitRouter* unit_router_;
    F05Config config_;

    // Lock-free counters (A-class stats).
    std::atomic<size_t> rejected_ns_count_{0};
    std::atomic<size_t> rejected_memory_count_{0};
    std::atomic<size_t> startup_load_failures_{0};

    // C-class ring buffer of the most recent 50 rejections.
    static constexpr size_t kMaxRejectionLog = 50;
    mutable std::mutex rejection_log_mutex_;
    std::deque<RejectionEvent> recent_rejections_;

    // Startup-failed NS list (C-class explain), guarded by pool_mutex_.
    std::vector<std::string> startup_failed_namespaces_;
};

}  // namespace cortrix::resource
