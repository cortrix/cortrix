#include <cstdint>
#include "cortrix/resource/namespace_pool.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/schema_provider.h"     // [D3.5-B] SchemaMigrator (MigrateUnit)
#include "cortrix/common/executor_engine.h"
#include "cortrix/id/types.h"  // ToSqliteInt (BlockId → SQLite int64, ARCH §1.8)
#include "cortrix/logging/logging.h"             // CORTRIX_LOG_WARN (step 8b)
#include "cortrix/memory/memory_store.h"         // step 8c memory.db pre-warm (D-I1.bis)
#include "cortrix/store/cortrix_store_sqlite.h"  // step 8b doc crash recovery view (D-I1.bis)
#include "cortrix/observability/observability_context.h"
#include "cortrix/resource/ns_pool_metrics.h"
#include "cortrix/store/f09_schema_provider.h"   // [D3.5-B] per-Unit framework provider
#include "cortrix/store/f34_schema_provider.h"   // [A unified-blocks] child cols + parents + indexes
#include "cortrix/spc_enricher/f03_schema_provider.h"  // [A unified-blocks] F03 enriched_score +3 + entities + FTS5 (§1.3.bis.3 #5)
#include "cortrix/scoring/scoring_schema_provider.h"   // [A unified-blocks] F07 semantic_score col (§1.3.bis.3 #6, gap-fill 2026-06-08)
#include "cortrix/doc_summary/f41_schema_provider.h"   // [A unified-blocks] F41 doc-level doc_fts5_index (§1.3.bis.3 #10)
#include "cortrix/doc_summary/doc_fts5_index.h"        // F41 rollback cleanup for doc_fts5_index rows
#include "cortrix/spc_enricher/f35_schema_provider.h"  // [A unified-blocks] contextualized/embedding cols
#include "cortrix/spc_enricher/enrich_state_schema_provider.h"  // enrich_state sidecar (coverage SoT)
#include "cortrix/retrieval/f40_schema_provider.h"     // [A unified-blocks] sparse_vec col + inverted index

namespace cortrix::resource {

namespace {

namespace obs = cortrix::observability;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Emit one OBS_SPEC §6 structured log line (§10.2). The metric *recorder*
// (cortrix_ns_pool_* counters/gauges/histograms, §10.1) is now implemented in
// ns_pool_metrics.{h,cpp} (NsPoolMetrics::Instance(), fed from this file); only
// registering it into the F24 `/metrics` scrape endpoint is still cross-Feature
// wiring → D3.5. The A-class values are also exposed via GetPoolStats() and the
// durations via StartupReport. Standalone we emit both the structured-log half
// (here) and the metric half (NsPoolMetrics feed points below).
void LogEvent(obs::LogLevel level, const std::string& event,
              const nlohmann::json& fields) {
    nlohmann::json line = fields;
    line["event"] = event;
    obs::ObservabilityContext::ThreadLocal().LogStructured(level, line.dump());
}

// Per-Unit on-disk layout for standalone D3: <data_root>/<unit_id>/ holds the
// index, pending.wal and store.db for that Unit. The real production scheme is
// owned by F12 / deployment (how the filesystem is laid out per Unit) and is an
// integration concern → resolved at D3.5; standalone derives it deterministically
// so the load path and its tests are reproducible.
std::string UnitDataDir(const std::string& data_root, const std::string& unit_id) {
    if (data_root.empty()) return unit_id;
    if (data_root.back() == '/') return data_root + unit_id;
    return data_root + "/" + unit_id;
}

std::string StoreDbPath(const std::string& unit_data_dir) {
    return unit_data_dir + "/store.db";
}

std::string UnitBlobDir(const std::string& unit_data_dir) {
    return unit_data_dir + "/blob";
}

// ── F25 Recover adapters (D3.5 C1, F05_NS_INTEGRATION §9.2) ──────────────────
// Two narrow, stateless probes the pool self-constructs per Unit so the
// WriteCoordinator's three-way Recover consistency check has its metadata + blob
// "does this exist?" answers WITHOUT requiring the façade layer (D-I4 unbroken).
// They borrow the resource layer's own backends: the store.db conn and the
// <unit_dir>/blob directory — see §9.2.

/// IMetadataStore over the Unit's store.db conn. BlockExists = "every block_id is
/// a row in `blocks`" via one COUNT(*) … IN (…) query. A prepare/step failure
/// (e.g. the blocks table not yet created on a fresh Unit) conservatively returns
/// false — Recover then treats the txn as not-fully-committed (the safe
/// direction: replay cleanup rather than keep a half-written doc).
class MetadataRecoverAdapter : public cortrix::store::IMetadataStore {
public:
    explicit MetadataRecoverAdapter(sqlite3* db) : db_(db) {}
    bool BlockExists(const std::vector<cortrix::BlockId>& block_ids) override {
        if (block_ids.empty()) return true;  // vacuously present
        if (!db_) return false;
        std::string sql = "SELECT COUNT(*) FROM blocks WHERE block_id IN (";
        for (size_t i = 0; i < block_ids.size(); ++i) sql += (i ? ",?" : "?");
        sql += ")";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return false;  // e.g. no `blocks` table yet on a fresh Unit
        }
        for (size_t i = 0; i < block_ids.size(); ++i) {
            sqlite3_bind_int64(stmt, static_cast<int>(i + 1),
                               cortrix::id::ToSqliteInt(block_ids[i]));
        }
        bool all_present = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const int64_t count = sqlite3_column_int64(stmt, 0);
            all_present = (count >= 0 &&
                           static_cast<size_t>(count) == block_ids.size());
        }
        sqlite3_finalize(stmt);
        return all_present;
    }

private:
    sqlite3* db_;  // borrowed (owned by the bundle's store_db)
};

/// IBlobStore over the Unit's blob dir: BlobExists = "<unit_dir>/blob/<doc_id>
/// exists on disk". Pure filesystem probe, no DB.
class BlobRecoverAdapter : public cortrix::store::IBlobStore {
public:
    explicit BlobRecoverAdapter(std::string blob_dir)
        : blob_dir_(std::move(blob_dir)) {}
    bool BlobExists(const std::string& doc_id) override {
        std::error_code ec;
        return std::filesystem::exists(blob_dir_ + "/" + doc_id, ec);
    }

private:
    std::string blob_dir_;
};

}  // namespace

DefaultNamespacePool::DefaultNamespacePool(
    cortrix::store::IIndexFactory* index_factory,
    WriteCoordinatorFactory write_coord_factory,
    cortrix::catalog::INSRouter* ns_router,
    cortrix::catalog::IUnitRouter* unit_router,
    F05Config config)
    : index_factory_(index_factory),
      write_coord_factory_(std::move(write_coord_factory)),
      ns_router_(ns_router),
      unit_router_(unit_router),
      config_(std::move(config)) {}

// ── helpers ──────────────────────────────────────────────────────────────────

size_t DefaultNamespacePool::CurrentMemoryUsedLocked() const {
    size_t used = 0;
    for (const auto& [ns_id, slot] : bundles_) {
        used += slot.bundle.TotalMemoryEstimateBytes();
    }
    return used;
}

void DefaultNamespacePool::RefreshPoolGauges() const {
    // §10.1 cortrix_ns_pool_size + cortrix_ns_pool_memory_budget_used_bytes — take
    // a snapshot of the resident pool under the shared lock and push it to the
    // gauges. Called after any pool mutation (admit / evict / startup).
    int64_t size = 0;
    int64_t used = 0;
    {
        std::shared_lock<std::shared_mutex> lock(pool_mutex_);
        size = static_cast<int64_t>(bundles_.size());
        used = static_cast<int64_t>(CurrentMemoryUsedLocked());
    }
    auto& m = NsPoolMetrics::Instance();
    m.SetSize(size);
    m.SetMemoryBudgetUsedBytes(used);
}

void DefaultNamespacePool::RecordRejection(const std::string& namespace_id,
                                           const char* reason, size_t budget_used,
                                           size_t budget_limit,
                                           size_t estimated_size) {
    RejectionEvent ev;
    ev.timestamp_ms = NowMs();
    ev.namespace_id = namespace_id;
    ev.reason = reason;
    ev.budget_used = budget_used;
    ev.budget_limit = budget_limit;
    ev.estimated_size = estimated_size;

    {
        std::lock_guard<std::mutex> lock(rejection_log_mutex_);
        recent_rejections_.push_back(ev);
        while (recent_rejections_.size() > kMaxRejectionLog) {
            recent_rejections_.pop_front();
        }
    }
    // §10.2 F05_POOL_REJECTION (WARN). Structured fields mirror the RejectionEvent.
    LogEvent(obs::LogLevel::kWarn, "F05_POOL_REJECTION",
             {{"namespace_id", ev.namespace_id},
              {"reason", ev.reason},
              {"budget_used", ev.budget_used},
              {"budget_limit", ev.budget_limit},
              {"estimated_size", ev.estimated_size}});
    // §10.1 cortrix_ns_pool_rejected_creates_total{reason}. The two callers pass
    // the exact reason strings that map 1:1 to the metric label enum.
    const std::string reason_str(reason);
    NsPoolMetrics::Instance().RecordRejectedCreate(
        reason_str == "memory_exceeded"
            ? NsPoolMetrics::RejectReason::kMemoryExceeded
            : NsPoolMetrics::RejectReason::kNsCountExceeded);
}

Result<NamespaceResourceBundle> DefaultNamespacePool::LoadOneNamespaceInner(
    const std::string& ns_id) {
    // §10.1 cortrix_ns_pool_ns_load_duration_seconds — time the per-NS load. This
    // is the single common load path (startup, admit-create, reload all reach it),
    // so every NS load is observed exactly once. Observed on every return via the
    // scope guard below (success or failure).
    const auto ns_load_start = std::chrono::steady_clock::now();
    struct ObserveOnExit {
        std::chrono::steady_clock::time_point start;
        ~ObserveOnExit() {
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                    .count();
            NsPoolMetrics::Instance().ObserveNsLoadDuration(secs);
        }
    } observe_guard{ns_load_start};

    // 1. Resolve the NS's active Unit via the F12 routers (§6.3 step 1).
    auto unit_res = ns_router_->GetActiveUnit(ns_id);
    if (!unit_res.ok()) {
        return PoolStatus(PoolErrorCode::kNsLoadFailed,
                          "get_active_unit: " + unit_res.status().message());
    }
    const cortrix::catalog::UnitDescriptor& unit = unit_res.value();
    const std::string unit_dir = UnitDataDir(config_.data_root, unit.unit_id);

    // 2. Open the F01 index handle (loads snapshot + WAL recovery) (§6.3 step 2).
    auto idx_res = index_factory_->Open(unit_dir);
    if (!idx_res.ok()) {
        return PoolStatus(PoolErrorCode::kNsLoadFailed,
                          "open_index: " + idx_res.status().message());
    }
    std::unique_ptr<cortrix::store::IIndex> index = std::move(idx_res.value());

    // 3. Open store.db + apply the 6 PRAGMAs — BEFORE the coordinator (D3.5 C1,
    //    §9.1 ordering fix): the metadata recover adapter borrows this conn and
    //    SetRollbackCallback needs it, so store.db must exist before the WC.
    auto store_db = std::make_unique<SqliteConn>();
    Status opened = store_db->Open(StoreDbPath(unit_dir));
    if (!opened.ok()) {
        return PoolStatus(PoolErrorCode::kNsLoadFailed,
                          "open_store_db: " + opened.message());
    }
    store_db->ApplyPragmas(config_.pragmas);  // tolerated on failure (§7.1)

    // 3b. [D3.5-B] Per-Unit schema migration (ARCH §1.3.bis.3 unified governance) —
    //     run ONCE here, BEFORE the recover adapters (step 4) and the WC Recover
    //     (step 8), so documents/blocks exist before any read/write or the rollback
    //     DELETE FROM blocks. F09SchemaProvider owns the framework schema; this
    //     replaces the MVP per-Acquire CortrixStoreSqlite::CreateTables on the
    //     production (external-conn) path.
    {
        cortrix::store::F09SchemaProvider f09_provider;
        cortrix::store::F34SchemaProvider f34_provider;  // [A unified-blocks] after F09 (needs blocks)
        cortrix::spc::F03SchemaProvider f03_provider;    // [A unified-blocks] enriched_score +3 cols + entities + FTS5
        cortrix::scoring::ScoringSchemaProvider f07_provider;  // [A unified-blocks] semantic_score col (§1.3.bis.3 #6, gap-fill 2026-06-08)
        cortrix::spc::F35SchemaProvider f35_provider;    // [A unified-blocks] contextualized/embedding cols
        cortrix::doc_summary::F41SchemaProvider f41_provider;  // [A unified-blocks] doc-level doc_fts5_index (block_type=17 reuses blocks)
        cortrix::retrieval::F40SchemaProvider f40_provider;  // [A unified-blocks] sparse_vec + inverted index
        cortrix::spc::EnrichStateSchemaProvider enrich_state_provider;  // enrich_state sidecar (coverage SoT)
        cortrix::catalog::SchemaMigrator unit_migrator;
        unit_migrator.Register(&f09_provider);   // #3 framework: documents/blocks/blocks_fts
        unit_migrator.Register(&f34_provider);   // #4 child cols + parents + idx (incl idx_blocks_meta_doc)
        unit_migrator.Register(&f03_provider);   // #5 blocks +enriched_score/at/metadata + entities + FTS5
        unit_migrator.Register(&f07_provider);   // #6 blocks +semantic_score + idx (ColumnExists guard → order-insensitive vs F03)
        unit_migrator.Register(&f35_provider);   // #7 blocks +embedding/contextualized_*
        unit_migrator.Register(&f41_provider);   // #10 doc_fts5_index (doc-level FTS5; doc_summary block reuses blocks)
        unit_migrator.Register(&f40_provider);   // #11 blocks +sparse_vec + sparse_inverted_index
        unit_migrator.Register(&enrich_state_provider);  // #12 enrich_state (standalone table, no blocks dep)
        Status migrated = unit_migrator.MigrateUnit(store_db->handle(), unit.unit_id);
        if (!migrated.ok()) {
            return PoolStatus(PoolErrorCode::kNsLoadFailed,
                              "migrate_unit: " + migrated.message());
        }
    }

    // 4. Self-construct the two narrow F25 Recover adapters (§9.2): metadata over
    //    the store.db conn, blob over <unit_dir>/blob. They go in the bundle so
    //    they outlive the coordinator that borrows them via raw pointers.
    auto meta_recover =
        std::make_unique<MetadataRecoverAdapter>(store_db->handle());
    auto blob_recover =
        std::make_unique<BlobRecoverAdapter>(UnitBlobDir(unit_dir));

    // 5. Cross-cast the index handle to IVectorStore for the coordinator (§9.3 ★
    //    type contract): bundle.index is an IIndex*, but WriteCoordinator needs an
    //    IVectorStore* — independent base classes (i_vector_store.h), so PHnsw's
    //    multiple inheritance requires an RTTI sibling cross-cast (static_cast is
    //    illegal here). A null result means the index backend lacks the write/
    //    recover surface → load failure rather than a later null deref.
    auto* vstore = dynamic_cast<cortrix::store::IVectorStore*>(index.get());
    if (!vstore) {
        return PoolStatus(PoolErrorCode::kNsLoadFailed, "index_not_ivector_store");
    }

    // 6. Construct the per-NS F25 WriteCoordinator over the three stores (factory
    //    news + Init()s it; §6.3 step 3 / §9.3 step 6).
    auto coord_res = write_coord_factory_(unit_dir, config_.write_coord_config,
                                          vstore, meta_recover.get(),
                                          blob_recover.get());
    if (!coord_res.ok()) {
        return PoolStatus(PoolErrorCode::kNsLoadFailed,
                          "open_pwl: " + coord_res.status().message());
    }
    std::unique_ptr<cortrix::store::WriteCoordinator> coord =
        std::move(coord_res.value());

    if (coord) {
        // 7. Register the rollback cleanup (§9.4) BEFORE Recover — Recover replays
        //    it for any PENDING-only txn that needs cleanup. Phase 1 cleanup =
        //    drop the vectors (idempotent MarkDelete) + delete the metadata blocks.
        //    Captures stay valid for the bundle's lifetime: moving the unique_ptrs
        //    below does not move the pointed-to objects, and `pwl` (declared last)
        //    destructs first, so the callback never fires after index/store_db die.
        sqlite3* db_handle = store_db->handle();
        coord->SetRollbackCallback(
            [vstore, db_handle](const cortrix::store::PendingEntry& e) -> Status {
                for (cortrix::BlockId bid : e.block_ids) {
                    vstore->MarkDelete(bid);  // idempotent (i_vector_store.h)
                }
                if (db_handle && !e.block_ids.empty()) {
                    std::string sql = "DELETE FROM blocks WHERE block_id IN (";
                    for (size_t i = 0; i < e.block_ids.size(); ++i)
                        sql += (i ? ",?" : "?");
                    sql += ")";
                    sqlite3_stmt* stmt = nullptr;
                    if (sqlite3_prepare_v2(db_handle, sql.c_str(), -1, &stmt,
                                           nullptr) == SQLITE_OK) {
                        for (size_t i = 0; i < e.block_ids.size(); ++i) {
                            sqlite3_bind_int64(
                                stmt, static_cast<int>(i + 1),
                                cortrix::id::ToSqliteInt(e.block_ids[i]));
                        }
                        sqlite3_step(stmt);  // idempotent: absent rows = no-op
                        sqlite3_finalize(stmt);
                    }
                }
                // [A unified-blocks Inc 4-3] The SPC ingest write also inserts the
                // doc's `parents` rows in the same txn (parent_insert); the rollback
                // must drop them too. doc-scoped DELETE is idempotent (absent rows =
                // no-op) and safe even for blob-only / metadata-only txns with no
                // parents. Runs on the same store.db handle as the blocks DELETE.
                if (db_handle && !e.doc_id.empty()) {
                    sqlite3_stmt* pstmt = nullptr;
                    if (sqlite3_prepare_v2(db_handle,
                                           "DELETE FROM parents WHERE doc_id = ?",
                                           -1, &pstmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(pstmt, 1, e.doc_id.c_str(), -1,
                                          SQLITE_TRANSIENT);
                        sqlite3_step(pstmt);  // idempotent
                        sqlite3_finalize(pstmt);
                    }
                }
                // [F44/F41 path validation] SPC now also syncs one F08-derived row
                // into doc_fts5_index for doc-level fallback retrieval. Rollback must
                // drop it by doc_id, otherwise failed ingest can leave a benchmark-visible
                // orphan doc candidate with no committed blocks.
                if (db_handle && !e.doc_id.empty()) {
                    (void)cortrix::doc_summary::DeleteDocFts5Row(db_handle, e.doc_id);
                }
                return Status::Ok();
            });

        // 8. Crash recovery — three stores now all non-null (§6.3 step 9).
        Status recovered = coord->Recover();
        if (!recovered.ok()) {
            return PoolStatus(PoolErrorCode::kNsLoadFailed,
                              "recover_pwl: " + recovered.message());
        }
    }

    // 8b. Doc-level crash recovery (§6.3.bis step 5.5, D-I1.bis) — AFTER the WC
    //     Recover (bottom-up: block-level three-way consistency first, then the
    //     doc state machine): reset 'processing' docs to 'pending' (+ drop their
    //     partial blocks) and complete 'deleting' cascades. Runs ONCE here on the
    //     assembly conn (single-threaded, before any request) via a borrowed-conn
    //     view — this used to live in CortrixStoreSqlite::Open() but never ran on
    //     the production path (the former external-conn mode skipped it).
    //     Failure tolerated: stuck docs degrade to a visible 'processing' state
    //     rather than blocking the NS load (same containment as ApplyPragmas).
    {
        cortrix::CortrixStoreSqlite recovery_view(store_db->handle());
        if (recovery_view.RecoverCrashedDocs() != 0) {
            CORTRIX_LOG_WARN("resource",
                             "doc crash recovery failed for unit {} (ns {}) — "
                             "stuck docs left as-is",
                             unit.unit_id, ns_id);
        }

        // 8c. Pre-warm <unit_dir>/memory.db (same D-I1.bis startup/connection
        //     split): ownership stays with the facade at request time (D-I4),
        //     but the ONE-SHOT first-init — schema DDL + MEM04 column migration +
        //     WAL conversion — runs here in the single-threaded assembly window.
        //     A fresh namespace's first concurrent requests used to race that
        //     init: the WAL conversion's exclusive lock hits SQLite's
        //     lock-upgrade deadlock avoidance (immediate SQLITE_BUSY, busy
        //     handler NOT invoked) and the ColumnExists/ADD COLUMN pair is
        //     check-then-act. After this warm-up every facade Init is a no-op
        //     path (tables/columns exist, journal already WAL).
        //     Failure tolerated: facades retain their own Init (lazy fallback).
        cortrix::MemoryStore memory_prewarm(recovery_view);
        Status warmed = memory_prewarm.Init(unit_dir + "/memory.db");
        if (!warmed.ok()) {
            CORTRIX_LOG_WARN("resource",
                             "memory.db pre-warm failed for unit {} (ns {}): {}",
                             unit.unit_id, ns_id, warmed.message());
        }
    }

    // 9. Assemble the bundle (§6.3 step 10). Move order mirrors the §9.3 field
    //    declaration order; pwl last so it destructs first.
    NamespaceResourceBundle bundle;
    bundle.namespace_id = ns_id;
    UnitResourceBundle ub;
    ub.unit_id = unit.unit_id;
    ub.data_dir = unit_dir;   // wire④: Facade sites blob/ + memory.db under here
    ub.index = std::move(index);
    ub.store_db = std::move(store_db);
    ub.meta_recover = std::move(meta_recover);
    ub.blob_recover = std::move(blob_recover);
    ub.pwl = std::move(coord);
    bundle.unit_bundles.push_back(std::move(ub));
    return Result<NamespaceResourceBundle>(std::move(bundle));
}

Result<NamespaceResourceBundle> DefaultNamespacePool::LoadOneNamespace(
    const std::string& ns_id) {
    // §6.3 C1 fix: enforce a hard per-NS timeout so one slow/hung NS load cannot
    // stall the whole startup. The inner load runs on its own async task; we wait
    // up to load_timeout_ms_per_ns for it. On timeout we deliberately do NOT
    // detach-and-kill — the future stays alive (its thread finishes in the
    // background and tidies its own partial state), we just don't wait for it and
    // don't admit this NS. A non-positive timeout means "no timeout" (wait fully).
    if (config_.load_timeout_ms_per_ns <= 0) {
        return LoadOneNamespaceInner(ns_id);
    }
    auto fut = std::async(std::launch::async,
                          [this, ns_id]() { return LoadOneNamespaceInner(ns_id); });
    if (fut.wait_for(std::chrono::milliseconds(config_.load_timeout_ms_per_ns)) ==
        std::future_status::timeout) {
        // The std::future dtor will block on the orphaned task, which would
        // re-introduce the stall we are guarding against. Move it onto a detached
        // waiter so this call returns immediately; the task owns only its own
        // resources and releases them when it finishes.
        std::thread([f = std::make_shared<std::future<Result<NamespaceResourceBundle>>>(
                         std::move(fut))]() mutable { f->wait(); })
            .detach();
        return PoolStatus(PoolErrorCode::kNsLoadFailed,
                          "load timed out after " +
                              std::to_string(config_.load_timeout_ms_per_ns) + "ms");
    }
    return fut.get();
}

// ── admission control (§5) ───────────────────────────────────────────────────

Status DefaultNamespacePool::AdmitCreate(const std::string& namespace_id,
                                         size_t estimated_size_bytes) {
    std::unique_lock<std::shared_mutex> lock(pool_mutex_);

    // Gate 1 — NS count (§5.1). Re-admitting an already-resident NS is not a quota
    // hit (idempotent create from a retried catalog op): only a *new* NS counts.
    // A slot still marked pending_delete (a DeleteNamespace racing this create) is
    // treated as NOT resident: we cancel the pending reap and reload below, so the
    // recreate wins with fresh resources rather than aliasing a being-torn-down
    // slot. (On the live path the catalog PK on the soft-deleted row blocks a
    // same-name recreate before reaching here, so this is a belt-and-suspenders
    // guard.)
    auto resident_it = bundles_.find(namespace_id);
    const bool already_resident =
        resident_it != bundles_.end() &&
        !resident_it->second.pending_delete.load(std::memory_order_acquire);
    if (!already_resident && bundles_.size() + 1 > config_.max_namespaces_per_instance) {
        rejected_ns_count_.fetch_add(1, std::memory_order_relaxed);
        RecordRejection(namespace_id, "ns_count_exceeded",
                        /*budget_used=*/0, /*budget_limit=*/0,
                        estimated_size_bytes);
        return PoolStatus(PoolErrorCode::kNsQuotaExceeded,
                          "namespace count " +
                              std::to_string(config_.max_namespaces_per_instance) +
                              " reached");
    }

    // Gate 2 — memory budget, only when enabled (§5.1). Uses the live sum of
    // resident bundles plus the caller's estimate for the new NS.
    if (config_.memory_budget_bytes > 0 && !already_resident) {
        const size_t used = CurrentMemoryUsedLocked();
        if (used + estimated_size_bytes > config_.memory_budget_bytes) {
            rejected_memory_count_.fetch_add(1, std::memory_order_relaxed);
            RecordRejection(namespace_id, "memory_exceeded", used,
                            config_.memory_budget_bytes, estimated_size_bytes);
            return PoolStatus(PoolErrorCode::kNsResourceBudgetExceeded,
                              "memory budget " +
                                  std::to_string(config_.memory_budget_bytes) +
                                  " bytes would be exceeded");
        }
    }

    if (already_resident) return Status::Ok();  // idempotent re-admit

    // Load the new NS into the pool (§5.1 "load new NS"). A load failure surfaces as
    // CX_ERR_NS_LOAD_FAILED (F12 maps the non-quota case to CX_ERR_NS_POOL_INTERNAL).
    auto loaded = LoadOneNamespaceInner(namespace_id);
    if (!loaded.ok()) {
        return loaded.status();
    }
    // Insert in place: construct (or reuse) the slot and move-assign just its
    // bundle, leaving the slot's refcount/pending_delete to reset to a fresh state.
    // Reusing a pending_delete slot (the recreate-races-delete guard above) cancels
    // its pending reap — the new resources own the slot now.
    NamespaceSlot& slot = bundles_[namespace_id];
    slot.bundle = std::move(loaded.value());
    slot.refcount.store(0, std::memory_order_relaxed);
    slot.pending_delete.store(false, std::memory_order_release);
    // §10.1 size / memory_budget_used gauges — set under the lock we already hold
    // (RefreshPoolGauges would re-lock → deadlock here).
    NsPoolMetrics::Instance().SetSize(static_cast<int64_t>(bundles_.size()));
    NsPoolMetrics::Instance().SetMemoryBudgetUsedBytes(
        static_cast<int64_t>(CurrentMemoryUsedLocked()));
    LogEvent(obs::LogLevel::kDebug, "F05_POOL_ADMIT_SUCCESS",
             {{"namespace_id", namespace_id},
              {"pool_size_after", bundles_.size()}});
    return Status::Ok();
}

// ── access (§3.2) ────────────────────────────────────────────────────────────

Result<NamespaceResourceBundle*> DefaultNamespacePool::Acquire(
    const std::string& namespace_id) {
    std::shared_lock<std::shared_mutex> lock(pool_mutex_);
    auto it = bundles_.find(namespace_id);
    if (it == bundles_.end()) {
        return PoolStatus(PoolErrorCode::kNsNotFound, namespace_id);
    }
    NamespaceSlot& slot = it->second;
    // Refuse an NS that is being torn down (EvictForDelete marked it). New borrows
    // must not revive a slot whose reap is pending, so the refcount can only fall to
    // zero. EvictForDelete takes the exclusive lock, so it cannot flip pending_delete
    // between this check and the increment below — the shared lock excludes it.
    if (slot.pending_delete.load(std::memory_order_acquire)) {
        return PoolStatus(PoolErrorCode::kNsNotFound, namespace_id);
    }
    // Take a reference: keeps the bundle alive past a racing EvictForDelete (which
    // will then defer its erase to the matching Release). The returned pointer is
    // node-stable (unordered_map) so it survives concurrent inserts/rehash.
    slot.refcount.fetch_add(1, std::memory_order_acq_rel);
    return Result<NamespaceResourceBundle*>(&slot.bundle);
}

void DefaultNamespacePool::Release(const std::string& namespace_id) {
    // Drop the reference taken by Acquire (no longer the Phase-1 no-op). If this is
    // the last reference of an NS already marked pending_delete (a DeleteNamespace
    // raced an in-flight borrow), reap the slot here — releasing index /
    // WriteCoordinator / store.db. The common case (refcount stays >0, or the NS
    // was never pending_delete) takes only the shared lock.
    bool maybe_reap = false;
    {
        std::shared_lock<std::shared_mutex> lock(pool_mutex_);
        auto it = bundles_.find(namespace_id);
        if (it == bundles_.end()) {
            return;  // safe no-op: NS not resident (already reaped / never present)
        }
        NamespaceSlot& slot = it->second;
        const int prev = slot.refcount.fetch_sub(1, std::memory_order_acq_rel);
        // prev <= 0 would mean an unbalanced Release; the gate relies on exact
        // Acquire/Release pairing (NamespaceFacade RAII), so we only reap on the
        // clean last-release transition (prev == 1 → now 0).
        maybe_reap = (prev == 1) &&
                     slot.pending_delete.load(std::memory_order_acquire);
    }
    if (!maybe_reap) return;

    // Last reader of a pending-delete NS: re-acquire exclusively and re-verify under
    // the write lock (a concurrent recreate via AdmitCreate could have replaced the
    // slot and cleared pending_delete — in which case we must NOT erase it).
    std::unique_lock<std::shared_mutex> lock(pool_mutex_);
    auto it = bundles_.find(namespace_id);
    if (it == bundles_.end()) return;
    NamespaceSlot& slot = it->second;
    if (slot.pending_delete.load(std::memory_order_acquire) &&
        slot.refcount.load(std::memory_order_acquire) == 0) {
        bundles_.erase(it);
        // §10.1 size / memory_budget_used gauges — set under the lock we already hold.
        NsPoolMetrics::Instance().SetSize(static_cast<int64_t>(bundles_.size()));
        NsPoolMetrics::Instance().SetMemoryBudgetUsedBytes(
            static_cast<int64_t>(CurrentMemoryUsedLocked()));
        LogEvent(obs::LogLevel::kDebug, "F05_POOL_EVICT_REAPED",
                 {{"namespace_id", namespace_id},
                  {"pool_size_after", bundles_.size()}});
    }
}

// ── unload (§5.2 / §13.1) ────────────────────────────────────────────────────

Status DefaultNamespacePool::EvictForDelete(const std::string& namespace_id) {
    std::unique_lock<std::shared_mutex> lock(pool_mutex_);
    auto it = bundles_.find(namespace_id);
    // Idempotent: evicting an absent NS is Ok (the F12 rollback path may call this
    // for an NS that never made it into the pool, §5.2).
    if (it == bundles_.end()) return Status::Ok();

    NamespaceSlot& slot = it->second;
    // Gate the erase on outstanding Acquire references. The exclusive lock excludes
    // every Acquire, so this refcount read is stable: no new reference can appear
    // while we decide. If readers are in flight, DEFER — mark pending_delete (which
    // blocks new Acquires) and let the last Release reap the slot. This is the fix
    // for the §13.1 DeleteNamespace UAF: erasing here while a NamespaceFacade holds
    // &slot.bundle would dangle its pointer for the rest of the request.
    if (slot.refcount.load(std::memory_order_acquire) > 0) {
        slot.pending_delete.store(true, std::memory_order_release);
        LogEvent(obs::LogLevel::kDebug, "F05_POOL_EVICT_DEFERRED",
                 {{"namespace_id", namespace_id},
                  {"refcount", slot.refcount.load(std::memory_order_acquire)}});
        return Status::Ok();
    }

    // No readers in flight → erase now. Dropping the bundle's unique_ptrs releases
    // index / WriteCoordinator / store.db here.
    bundles_.erase(it);
    // §10.1 size / memory_budget_used gauges — set under the lock we already hold.
    NsPoolMetrics::Instance().SetSize(static_cast<int64_t>(bundles_.size()));
    NsPoolMetrics::Instance().SetMemoryBudgetUsedBytes(
        static_cast<int64_t>(CurrentMemoryUsedLocked()));
    return Status::Ok();
}

// ── startup (§6) — S1 baseline; 8-worker concurrency + timeout land in S2 ─────

Result<StartupReport> DefaultNamespacePool::StartupLoadAll() {
    StartupReport report;
    const auto start = std::chrono::steady_clock::now();

    auto list_res = ns_router_->ListNamespaces({});
    if (!list_res.ok()) {
        return list_res.status();
    }
    std::vector<std::string> ns_list = list_res.value().results;

    // Startup admission backstop (§6.2): never load past the NS-count ceiling.
    if (ns_list.size() > config_.max_namespaces_per_instance) {
        LogEvent(obs::LogLevel::kError, "F05_POOL_STARTUP_TRUNCATED",
                 {{"catalog_ns_count", ns_list.size()},
                  {"max_namespaces", config_.max_namespaces_per_instance}});
        ns_list.resize(config_.max_namespaces_per_instance);
    }
    report.total_namespaces = ns_list.size();

    // Eager concurrent load on the shared bounded-queue thread pool (§6.2). The
    // queue is sized to hold every NS so Submit never blocks the startup thread;
    // `startup_load_workers` (default 8) caps real concurrency. Each task runs the
    // timeout-guarded LoadOneNamespace (§6.3 C1). A per-NS load failure is recorded
    // and does NOT abort the others (UT11) — partial failure is reported via the
    // StartupReport + admin ReloadNamespace.
    const int workers = config_.startup_load_workers > 0
                            ? config_.startup_load_workers
                            : 1;
    const int queue_size =
        ns_list.empty() ? 1 : static_cast<int>(ns_list.size());
    cortrix::ExecutorEngine exec(workers, queue_size);

    std::vector<std::future<Result<NamespaceResourceBundle>>> futures;
    futures.reserve(ns_list.size());
    for (const std::string& ns_id : ns_list) {
        futures.push_back(exec.Submit<Result<NamespaceResourceBundle>>(
            [this, ns_id]() { return LoadOneNamespace(ns_id); }));
    }

    for (size_t i = 0; i < ns_list.size(); ++i) {
        Result<NamespaceResourceBundle> loaded = futures[i].get();
        if (loaded.ok()) {
            std::unique_lock<std::shared_mutex> lock(pool_mutex_);
            // Fresh slot (refcount 0, not pending_delete); move just the bundle in.
            bundles_[ns_list[i]].bundle = std::move(loaded.value());
            report.loaded_successfully++;
        } else {
            startup_load_failures_.fetch_add(1, std::memory_order_relaxed);
            // §10.1 cortrix_ns_pool_startup_load_failures_total.
            NsPoolMetrics::Instance().AddStartupLoadFailures(1);
            report.failed++;
            report.failed_namespaces.push_back(ns_list[i]);
            {
                std::unique_lock<std::shared_mutex> lock(pool_mutex_);
                startup_failed_namespaces_.push_back(ns_list[i]);
            }
            LogEvent(obs::LogLevel::kError, "F05_POOL_NS_LOAD_FAILED",
                     {{"namespace_id", ns_list[i]},
                      {"error_detail", loaded.status().message()}});
        }
    }

    report.total_duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    // §10.1 cortrix_ns_pool_startup_load_duration_seconds (whole-pool startup) +
    // refresh the size / memory_budget_used gauges to the post-startup state.
    NsPoolMetrics::Instance().ObserveStartupLoadDuration(
        static_cast<double>(report.total_duration_ms) / 1000.0);
    RefreshPoolGauges();
    LogEvent(obs::LogLevel::kInfo, "F05_POOL_STARTUP_COMPLETE",
             {{"total_namespaces", report.total_namespaces},
              {"loaded_successfully", report.loaded_successfully},
              {"failed", report.failed},
              {"total_duration_ms", report.total_duration_ms}});
    return Result<StartupReport>(std::move(report));
}

Status DefaultNamespacePool::ReloadNamespace(const std::string& namespace_id) {
    // Recovery path: load WITHOUT the startup timeout (LoadOneNamespaceInner, not the
    // timeout-guarded LoadOneNamespace). ReloadNamespace exists precisely to admit an
    // NS the bounded startup load could not finish in time (a large NS); reusing the
    // same per-NS timeout here would make recovery impossible for exactly those NSs.
    // This is a deliberate, admin/on-demand call (not the concurrent startup sweep),
    // so blocking until the load completes is correct.
    auto loaded = LoadOneNamespaceInner(namespace_id);
    if (!loaded.ok()) {
        LogEvent(obs::LogLevel::kError, "F05_POOL_NS_LOAD_FAILED",
                 {{"namespace_id", namespace_id},
                  {"error_detail", loaded.status().message()}});
        return loaded.status();
    }
    std::unique_lock<std::shared_mutex> lock(pool_mutex_);
    // Reload replaces the bundle in place (or creates a fresh slot). A reload only
    // applies to a load-failed NS — which was never inserted, so refcount is 0 and
    // there is no pending_delete to worry about; assign just the bundle field.
    bundles_[namespace_id].bundle = std::move(loaded.value());
    // Clear it from the startup-failed list on a successful reload (§8.3.3).
    for (auto it = startup_failed_namespaces_.begin();
         it != startup_failed_namespaces_.end();) {
        it = (*it == namespace_id) ? startup_failed_namespaces_.erase(it)
                                   : it + 1;
    }
    // §10.1 size / memory_budget_used gauges — set under the lock we already hold.
    NsPoolMetrics::Instance().SetSize(static_cast<int64_t>(bundles_.size()));
    NsPoolMetrics::Instance().SetMemoryBudgetUsedBytes(
        static_cast<int64_t>(CurrentMemoryUsedLocked()));
    return Status::Ok();
}

// ── state queries (§3.2 / §9) ────────────────────────────────────────────────

PoolStats DefaultNamespacePool::GetPoolStats() const {
    PoolStats stats;
    {
        std::shared_lock<std::shared_mutex> lock(pool_mutex_);
        stats.pool_size_current = bundles_.size();
        stats.memory_budget_used_bytes = CurrentMemoryUsedLocked();
    }
    stats.pool_size_max = config_.max_namespaces_per_instance;
    stats.memory_budget_limit_bytes = config_.memory_budget_bytes;
    stats.rejected_creates_total.ns_count_exceeded =
        rejected_ns_count_.load(std::memory_order_relaxed);
    stats.rejected_creates_total.memory_exceeded =
        rejected_memory_count_.load(std::memory_order_relaxed);
    stats.startup_load_failures =
        startup_load_failures_.load(std::memory_order_relaxed);
    return stats;
}

ExplainState DefaultNamespacePool::GetExplainState() const {
    ExplainState state;
    {
        std::shared_lock<std::shared_mutex> lock(pool_mutex_);
        state.active_namespaces.reserve(bundles_.size());
        for (const auto& [ns_id, slot] : bundles_) {
            (void)slot;
            state.active_namespaces.push_back(ns_id);
        }
        state.memory_estimate_bytes = CurrentMemoryUsedLocked();
        state.startup_failed_namespaces = startup_failed_namespaces_;
    }
    {
        std::lock_guard<std::mutex> lock(rejection_log_mutex_);
        state.recent_rejections.assign(recent_rejections_.begin(),
                                       recent_rejections_.end());
    }
    return state;
}

}  // namespace cortrix::resource
