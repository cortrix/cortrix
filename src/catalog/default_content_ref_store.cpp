#include "cortrix/catalog/default_content_ref_store.h"

#include <sqlite3.h>

#include <chrono>
#include <string>
#include <vector>

#include "cortrix/catalog/catalog_error.h"

namespace cortrix::catalog {

namespace {

int64_t WallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void BindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

// Run a parameter-less statement, returning false on error.
bool ExecOk(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

}  // namespace

DefaultContentRefStore::DefaultContentRefStore(sqlite3* db) : db_(db) {}

Status DefaultContentRefStore::IncrementRef(const std::string& file_hash,
                                            const std::string& ns_id,
                                            const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ExecOk(db_, "BEGIN")) {
        return CatalogStatus(CatalogErrorCode::kTransactionRetry, "begin failed");
    }

    // The (file,ns,doc) content_refs row is authoritative. A reference is
    // "genuinely new" only if no ACTIVE row for the triple already exists — that
    // is the condition under which the file_locations aggregate is bumped.
    // (ON CONFLICT DO UPDATE always reports a change, so it can't tell a new ref
    // from a re-reference; we check the prior active state explicitly.)
    bool already_active = false;
    {
        const char* sql =
            "SELECT 1 FROM content_refs "
            "WHERE file_hash=? AND ns_id=? AND doc_id=? AND status='active'";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare IncrementRef probe: " + m);
        }
        BindText(stmt, 1, file_hash);
        BindText(stmt, 2, ns_id);
        BindText(stmt, 3, doc_id);
        already_active = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    const bool newly_added = !already_active;

    // Upsert the row active (idempotent for an existing active row; reactivates a
    // previously soft-deleted one).
    if (newly_added) {
        const char* sql =
            "INSERT INTO content_refs (file_hash, ns_id, doc_id, target_unit_id, "
            "status, created_at) VALUES (?, ?, ?, '', 'active', ?) "
            "ON CONFLICT(file_hash, ns_id, doc_id) DO UPDATE SET status='active'";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare IncrementRef: " + m);
        }
        BindText(stmt, 1, file_hash);
        BindText(stmt, 2, ns_id);
        BindText(stmt, 3, doc_id);
        sqlite3_bind_int64(stmt, 4, WallMs());
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "IncrementRef upsert failed");
        }
    }

    // Keep the file_locations aggregate in sync IF that row exists (it is created
    // by the write path, F25). Only bump on a genuinely new reference.
    if (newly_added) {
        const char* sql =
            "UPDATE file_locations SET ref_count = ref_count + 1 WHERE file_hash = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare ref_count++: " + m);
        }
        BindText(stmt, 1, file_hash);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "ref_count++ failed");
        }
    }

    if (!ExecOk(db_, "COMMIT")) {
        ExecOk(db_, "ROLLBACK");
        return CatalogStatus(CatalogErrorCode::kTransactionRetry, "commit failed");
    }
    return Status::Ok();
}

Result<int> DefaultContentRefStore::DecrementRef(const std::string& file_hash,
                                                 const std::string& ns_id,
                                                 const std::string& doc_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ExecOk(db_, "BEGIN")) {
        return CatalogStatus(CatalogErrorCode::kTransactionRetry, "begin failed");
    }

    // Soft-delete the (file,ns,doc) row if it is currently active.
    bool removed = false;
    {
        const char* sql =
            "UPDATE content_refs SET status='deleted' "
            "WHERE file_hash=? AND ns_id=? AND doc_id=? AND status='active'";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::string m = sqlite3_errmsg(db_);
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "prepare DecrementRef: " + m);
        }
        BindText(stmt, 1, file_hash);
        BindText(stmt, 2, ns_id);
        BindText(stmt, 3, doc_id);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kInternalError, "DecrementRef update failed");
        }
        removed = sqlite3_changes(db_) > 0;
    }

    // Decrement the file_locations aggregate (guard against going negative).
    if (removed) {
        // Read current count first to enforce the non-negative invariant.
        int current = -1;
        {
            const char* sql = "SELECT ref_count FROM file_locations WHERE file_hash=?";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                BindText(stmt, 1, file_hash);
                if (sqlite3_step(stmt) == SQLITE_ROW) current = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
            }
        }
        if (current == 0) {
            ExecOk(db_, "ROLLBACK");
            return CatalogStatus(CatalogErrorCode::kRefCountNegative,
                                 "file '" + file_hash + "' ref_count already 0");
        }
        if (current > 0) {
            const char* sql =
                "UPDATE file_locations SET ref_count = ref_count - 1 WHERE file_hash=?";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                std::string m = sqlite3_errmsg(db_);
                ExecOk(db_, "ROLLBACK");
                return CatalogStatus(CatalogErrorCode::kInternalError, "prepare ref_count--: " + m);
            }
            BindText(stmt, 1, file_hash);
            const int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                ExecOk(db_, "ROLLBACK");
                return CatalogStatus(CatalogErrorCode::kInternalError, "ref_count-- failed");
            }
        }
    }

    if (!ExecOk(db_, "COMMIT")) {
        ExecOk(db_, "ROLLBACK");
        return CatalogStatus(CatalogErrorCode::kTransactionRetry, "commit failed");
    }

    // Return the resulting count (0 if no file_locations row tracked). We already
    // hold mu_, so use the lock-free variant (calling GetRefCount here would
    // re-lock the non-recursive mutex and deadlock).
    return GetRefCountLocked(file_hash);
}

Result<int> DefaultContentRefStore::GetRefCount(const std::string& file_hash) const {
    std::lock_guard<std::mutex> lock(mu_);
    return GetRefCountLocked(file_hash);
}

Result<int> DefaultContentRefStore::GetRefCountLocked(const std::string& file_hash) const {
    const char* sql = "SELECT ref_count FROM file_locations WHERE file_hash = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare GetRefCount: ") + sqlite3_errmsg(db_));
    }
    BindText(stmt, 1, file_hash);
    int count = 0;  // unknown file → 0
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

Result<std::vector<std::string>> DefaultContentRefStore::GetGCCandidates(
    int64_t before_timestamp_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    // OPEN-2 Stage 3: files whose ref_count has dropped to 0 and were soft-deleted
    // before the cutoff (deleted_at < before). status='deleted' marks Stage 1.
    const char* sql =
        "SELECT file_hash FROM file_locations "
        "WHERE ref_count <= 0 AND status='deleted' "
        "AND deleted_at IS NOT NULL AND deleted_at < ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return CatalogStatus(CatalogErrorCode::kInternalError,
                             std::string("prepare GetGCCandidates: ") + sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(stmt, 1, before_timestamp_ms);
    std::vector<std::string> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (t) out.emplace_back(t);
    }
    sqlite3_finalize(stmt);
    return out;
}

}  // namespace cortrix::catalog
