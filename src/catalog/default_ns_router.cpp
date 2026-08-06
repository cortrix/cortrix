#include <cstdint>
#include "cortrix/catalog/default_ns_router.h"

#include <sqlite3.h>

#include <chrono>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_error.h"
#include "cortrix/logging/logging.h"           // CORTRIX_LOG_WARN (§3.1.bis evict-fail log)
#include "cortrix/resource/namespace_pool.h"  // Complete INamespacePool for AdmitCreate/EvictForDelete

namespace cortrix::catalog {

namespace {

// §6.3 cache sizing.
constexpr std::size_t kNsCacheCapacity   = 10000;
constexpr std::size_t kUnitCacheCapacity = 1000;
constexpr int64_t     kCacheTtlMs        = 60000;  // 60s

// Monotonic millisecond clock for cache TTL (steady, not wall-clock).
int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Wall-clock millis for stored created_at timestamps (schema uses unix ms).
int64_t WallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// TEXT column → std::string (NULL-safe).
std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

// TEXT column → optional<string> (NULL → nullopt).
std::optional<std::string> ColTextOpt(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return ColText(stmt, col);
}

// INTEGER column → optional<int64> (NULL → nullopt).
std::optional<int64_t> ColIntOpt(sqlite3_stmt* stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
    return static_cast<int64_t>(sqlite3_column_int64(stmt, col));
}

void BindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

}  // namespace

DefaultINSRouter::DefaultINSRouter(sqlite3* db, resource::INamespacePool* ns_pool)
    : db_(db),
      ns_pool_(ns_pool),
      ns_cache_(kNsCacheCapacity, kCacheTtlMs),
      unit_cache_(kUnitCacheCapacity, kCacheTtlMs) {}

// --- private readers (caller holds mu_) -------------------------------------

Result<NSMetadata> DefaultINSRouter::ReadNamespaceLocked(
    const std::string& ns_id) const {
    const char* sql =
        "SELECT ns_id, name, tenant_id, isolation_mode, visibility, owner_user_id, "
        "cloned_from_ns_id, clone_type, cloned_at, status, created_at, "
        "reranker_config, enricher_config, parser_config, memory_config, "
        "watcher_config, cleaning_config, rag_fusion_config, crag_config, "
        "complexity_config, sparse_config, doc_summary_config "
        "FROM namespaces WHERE ns_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare GetNamespace: ") + sqlite3_errmsg(db_));
    }
    BindText(stmt, 1, ns_id);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CatalogStatus(CatalogErrorCode::kNsNotFound, "namespace '" + ns_id + "'");
    }
    NSMetadata m;
    m.namespace_id      = ColText(stmt, 0);
    m.name              = ColText(stmt, 1);
    m.tenant_id         = ColText(stmt, 2);
    m.isolation_mode    = ColText(stmt, 3);
    m.visibility        = ColText(stmt, 4);
    m.owner_user_id     = ColTextOpt(stmt, 5);
    m.cloned_from_ns_id = ColTextOpt(stmt, 6);
    m.clone_type        = ColTextOpt(stmt, 7);
    m.cloned_at         = ColIntOpt(stmt, 8);
    m.status            = ColText(stmt, 9);
    m.created_at        = sqlite3_column_int64(stmt, 10);
    m.reranker_config    = ColTextOpt(stmt, 11);
    m.enricher_config    = ColTextOpt(stmt, 12);
    m.parser_config      = ColTextOpt(stmt, 13);
    m.memory_config      = ColTextOpt(stmt, 14);
    m.watcher_config     = ColTextOpt(stmt, 15);
    m.cleaning_config    = ColTextOpt(stmt, 16);
    m.rag_fusion_config  = ColTextOpt(stmt, 17);
    m.crag_config        = ColTextOpt(stmt, 18);
    m.complexity_config  = ColTextOpt(stmt, 19);
    m.sparse_config      = ColTextOpt(stmt, 20);
    m.doc_summary_config = ColTextOpt(stmt, 21);
    sqlite3_finalize(stmt);
    return m;
}

Result<UnitDescriptor> DefaultINSRouter::ReadUnitLocked(
    const std::string& unit_id) const {
    const char* sql =
        "SELECT unit_id, tenant_id, state, index_type, sealed_at, archived_at, "
        "vector_count, size_bytes, last_write_at, seal_threshold_vectors, "
        "seal_threshold_bytes, seal_idle_days, owner_node_id, created_at "
        "FROM units WHERE unit_id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare GetUnit: ") + sqlite3_errmsg(db_));
    }
    BindText(stmt, 1, unit_id);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CatalogStatus(CatalogErrorCode::kUnitNotFound, "unit '" + unit_id + "'");
    }
    UnitDescriptor u;
    u.unit_id       = ColText(stmt, 0);
    u.tenant_id     = ColText(stmt, 1);
    u.state         = UnitStateFromString(ColText(stmt, 2));
    u.index_type    = IndexTypeFromString(ColText(stmt, 3));
    u.sealed_at     = ColIntOpt(stmt, 4);
    u.archived_at   = ColIntOpt(stmt, 5);
    u.vector_count  = sqlite3_column_int64(stmt, 6);
    u.size_bytes    = sqlite3_column_int64(stmt, 7);
    u.last_write_at = sqlite3_column_int64(stmt, 8);
    u.seal_threshold_vectors = sqlite3_column_int64(stmt, 9);
    u.seal_threshold_bytes   = sqlite3_column_int64(stmt, 10);
    u.seal_idle_days = static_cast<int32_t>(sqlite3_column_int(stmt, 11));
    u.owner_node_id = ColText(stmt, 12);
    u.created_at    = sqlite3_column_int64(stmt, 13);
    sqlite3_finalize(stmt);
    return u;
}

Result<std::string> DefaultINSRouter::ActiveUnitIdLocked(
    const std::string& ns_id) const {
    // Phase 1: 1 NS = 1 Unit → the single (is_primary) mapping row. We first
    // confirm the NS exists to return CX_ERR_NS_NOT_FOUND rather than a vague
    // "no unit" when the namespace itself is absent.
    const char* sql =
        "SELECT unit_id FROM ns_units WHERE ns_id = ? "
        "ORDER BY is_primary DESC, created_at ASC LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare LookupUnits: ") + sqlite3_errmsg(db_));
    }
    BindText(stmt, 1, ns_id);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CatalogStatus(CatalogErrorCode::kNsNotFound, "namespace '" + ns_id + "'");
    }
    std::string unit_id = ColText(stmt, 0);
    sqlite3_finalize(stmt);
    return unit_id;
}

// --- INSRouter surface ------------------------------------------------------

Result<NSMetadata> DefaultINSRouter::GetNamespace(
    const std::string& namespace_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = NowMs();
    if (auto hit = ns_cache_.Get(namespace_id, now)) return *hit;
    Result<NSMetadata> r = ReadNamespaceLocked(namespace_id);
    if (r.ok()) ns_cache_.Put(namespace_id, r.value(), now);
    return r;
}

Result<UnitDescriptor> DefaultINSRouter::GetActiveUnit(
    const std::string& namespace_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = NowMs();
    Result<std::string> unit_id = ActiveUnitIdLocked(namespace_id);
    if (!unit_id.ok()) return unit_id.status();
    if (auto hit = unit_cache_.Get(unit_id.value(), now)) return *hit;
    Result<UnitDescriptor> u = ReadUnitLocked(unit_id.value());
    if (u.ok()) unit_cache_.Put(unit_id.value(), u.value(), now);
    return u;
}

Result<std::vector<UnitDescriptor>> DefaultINSRouter::LookupUnits(
    const std::string& namespace_id) const {
    // Phase 1: exactly one Unit. GetActiveUnit already does NS-exists + cache.
    Result<UnitDescriptor> u = GetActiveUnit(namespace_id);
    if (!u.ok()) return u.status();
    return std::vector<UnitDescriptor>{u.value()};
}

Result<BatchResult<std::string>> DefaultINSRouter::ListNamespaces(
    const ListNamespacesOptions& opts) const {
    std::lock_guard<std::mutex> lock(mu_);

    std::string sql = "SELECT ns_id FROM namespaces WHERE 1=1";
    if (opts.tenant_id.has_value())  sql += " AND tenant_id = ?";
    if (!opts.include_deleted)       sql += " AND status != 'deleted'";
    sql += " ORDER BY ns_id ASC";
    if (opts.limit > 0)  sql += " LIMIT " + std::to_string(opts.limit);
    if (opts.offset > 0) sql += " OFFSET " + std::to_string(opts.offset);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare ListNamespaces: ") + sqlite3_errmsg(db_));
    }
    if (opts.tenant_id.has_value()) BindText(stmt, 1, *opts.tenant_id);

    BatchResult<std::string> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.results.push_back(ColText(stmt, 0));
    }
    sqlite3_finalize(stmt);

    out.meta.succeeded_ids = out.results;
    out.meta.coverage_ratio = 1.0f;  // no per-item failures in a plain list
    out.meta.timestamp_ms = WallMs();
    out.meta.node_id = "local";
    return out;
}

Status DefaultINSRouter::CreateNamespace(const NSMetadata& metadata) {
    if (metadata.namespace_id.empty()) {
        return CatalogStatus(CatalogErrorCode::kInvalidConfigJson, "ns_id is empty");
    }
    // Catalog INSERT first, then pool admission.
    // The original §5.1 order (admit → insert) was a dependency knot: AdmitCreate's
    // load calls GetActiveUnit, which reads the very ns_units row this INSERT
    // creates — fresh creates always failed NS_NOT_FOUND (masked by mock routers /
    // null pools in unit tests). Wrapping the admit inside the catalog txn is
    // lock-unsafe (GetActiveUnit re-enters mu_; a recursive lock would cross
    // mu_→pool against the regular pool→mu_ order = AB-BA). So: commit the row
    // first (own txn, mu_ released), then admit (lock order stays pool→mu_,
    // one-way), and compensate with DeleteNamespace on rejection — symmetric to
    // the §5.2 EvictForDelete pattern. A crash between the two self-heals:
    // StartupLoadAll picks the orphan row up on the next boot.
    Status catalog = InsertNamespaceCatalog(metadata);
    if (!catalog.ok()) return catalog;

    if (ns_pool_ != nullptr) {
        Status admit = ns_pool_->AdmitCreate(metadata.namespace_id, /*estimated_size_bytes=*/0);
        if (!admit.ok()) {  // CX_ERR_NS_QUOTA_EXCEEDED / RESOURCE_BUDGET / LOAD_FAILED
            RemoveNamespaceCatalogRows(metadata.namespace_id);
            return admit;
        }
    }
    return Status::Ok();
}

// §12.2②: INSERT the namespace row + its sole Unit + the 1:1 ns_units mapping in
// one txn. Extracted from CreateNamespace so the pool admission/rollback wraps it.
Status DefaultINSRouter::InsertNamespaceCatalog(const NSMetadata& metadata) {
    std::lock_guard<std::mutex> lock(mu_);

    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "begin failed";
        sqlite3_free(err);
        return CatalogStatus(CatalogErrorCode::kTransactionRetry, m);
    }

    const int64_t now = WallMs();
    // INSERT the namespace row (defaults fill the 11 *_config + isolation cols).
    {
        const char* sql =
            "INSERT INTO namespaces (ns_id, name, tenant_id, isolation_mode, "
            "visibility, owner_user_id, status, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare insert ns: " + m);
        }
        BindText(stmt, 1, metadata.namespace_id);
        BindText(stmt, 2, metadata.name);
        BindText(stmt, 3, metadata.tenant_id);
        BindText(stmt, 4, metadata.isolation_mode.empty() ? "shared" : metadata.isolation_mode);
        BindText(stmt, 5, metadata.visibility.empty() ? "tenant" : metadata.visibility);
        if (metadata.owner_user_id.has_value()) BindText(stmt, 6, *metadata.owner_user_id);
        else sqlite3_bind_null(stmt, 6);
        BindText(stmt, 7, metadata.status.empty() ? "active" : metadata.status);
        sqlite3_bind_int64(stmt, 8, now);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            const int ext = sqlite3_extended_errcode(db_);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            // UNIQUE/PK violation → already exists.
            if (ext == SQLITE_CONSTRAINT_PRIMARYKEY || ext == SQLITE_CONSTRAINT_UNIQUE) {
                return CatalogStatus(CatalogErrorCode::kNsAlreadyExists,
                                     "namespace '" + metadata.namespace_id + "'");
            }
            return CatalogStatus(CatalogErrorCode::kInternalError, "insert ns failed");
        }
    }

    // Phase 1: create the NS's sole Unit + the 1:1 ns_units mapping. Unit id is
    // derived deterministically from the ns_id (single-Unit invariant).
    const std::string unit_id = "unit-" + metadata.namespace_id;
    {
        const char* sql =
            "INSERT INTO units (unit_id, tenant_id, created_at, last_write_at) "
            "VALUES (?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare insert unit: " + m);
        }
        BindText(stmt, 1, unit_id);
        BindText(stmt, 2, metadata.tenant_id);
        sqlite3_bind_int64(stmt, 3, now);
        sqlite3_bind_int64(stmt, 4, now);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return CatalogStatus(CatalogErrorCode::kInternalError, "insert unit failed");
        }
    }
    {
        const char* sql =
            "INSERT INTO ns_units (ns_id, unit_id, is_primary, created_at) "
            "VALUES (?, ?, 1, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare insert ns_units: " + m);
        }
        BindText(stmt, 1, metadata.namespace_id);
        BindText(stmt, 2, unit_id);
        sqlite3_bind_int64(stmt, 3, now);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return CatalogStatus(CatalogErrorCode::kInternalError, "insert ns_units failed");
        }
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "commit failed";
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return CatalogStatus(CatalogErrorCode::kTransactionRetry, m);
    }
    return Status::Ok();
}

void DefaultINSRouter::RemoveNamespaceCatalogRows(const std::string& ns_id) {
    std::lock_guard<std::mutex> lock(mu_);
    // Best-effort hard removal in one txn (units via the still-present mapping,
    // then the mapping, then the ns row). A failure here leaves an orphan row
    // that the next StartupLoadAll load attempt surfaces — same self-heal window
    // as a crash between insert and admit.
    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return;
    }
    const char* stmts[] = {
        "DELETE FROM units WHERE unit_id IN (SELECT unit_id FROM ns_units WHERE ns_id = ?)",
        "DELETE FROM ns_units WHERE ns_id = ?",
        "DELETE FROM namespaces WHERE ns_id = ?",
    };
    bool ok = true;
    for (const char* sql : stmts) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { ok = false; break; }
        BindText(stmt, 1, ns_id);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) { ok = false; break; }
    }
    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
    ns_cache_.Invalidate(ns_id);
}

Status DefaultINSRouter::DeleteNamespace(const std::string& namespace_id) {
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql =
        "UPDATE namespaces SET status = 'deleted' "
        "WHERE ns_id = ? AND status != 'deleted'";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare DeleteNamespace: ") + sqlite3_errmsg(db_));
    }
    BindText(stmt, 1, namespace_id);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return CatalogStatus(CatalogErrorCode::kInternalError, "soft-delete ns failed");
    }
    if (sqlite3_changes(db_) == 0) {
        // Either the NS does not exist, or it was already deleted → not found.
        return CatalogStatus(CatalogErrorCode::kNsNotFound, "namespace '" + namespace_id + "'");
    }
    // Invalidate caches so a stale active row isn't served post-delete.
    ns_cache_.Invalidate(namespace_id);

    // Pool eviction hook: after the catalog soft-delete,
    // release the NS's pool resources (index / WriteCoordinator / store.db). When
    // ns_pool_ is null (Phase 1 standalone / catalog-standalone) this is skipped. Per
    // §3.1.bis the catalog delete is authoritative: an EvictForDelete failure is
    // logged but NOT rolled back (any pool residue is reclaimable via the admin API,
    // and self-heals on restart since StartupLoadAll skips deleted NS). EvictForDelete
    // itself is UAF-safe — if a request still holds the NS it defers the actual erase
    // to the last Release (pool refcount gate).
    if (ns_pool_ != nullptr) {
        const Status evict = ns_pool_->EvictForDelete(namespace_id);
        if (!evict.ok()) {
            CORTRIX_LOG_WARN("catalog.DeleteNamespace",
                             "catalog soft-deleted but namespace pool EvictForDelete failed: "
                             "ns={}, reason={}",
                             namespace_id, evict.message());
        }
    }
    return Status::Ok();
}

}  // namespace cortrix::catalog
