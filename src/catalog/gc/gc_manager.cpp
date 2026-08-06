#include <cstdint>
#include "cortrix/catalog/gc/gc_manager.h"

#include <sqlite3.h>

#include <chrono>
#include <string>
#include <vector>

namespace cortrix::catalog::gc {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t DaysToMs(int days) { return static_cast<int64_t>(days) * 24 * 3600 * 1000; }

void BindText(sqlite3_stmt* stmt, int idx, const std::string& v) {
    sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

bool ExecOk(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

}  // namespace

GcManager::GcManager(sqlite3* db, IBlobGcSink* sink, const GcConfig& config)
    : db_(db), sink_(sink), config_(config) {}

Result<GcStatusSnapshot> GcManager::GetStatus() const {
    std::lock_guard<std::mutex> lock(mu_);
    GcStatusSnapshot s;
    s.running = running_;
    s.last_gc_at_ms = last_gc_at_ms_;

    // Stage 1 backlog: file_locations rows currently soft-deleted.
    {
        const char* sql =
            "SELECT COUNT(*) FROM file_locations WHERE status='deleted'";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("CX_ERR_GC_INTERNAL: status count: ") +
                                    sqlite3_errmsg(db_));
        }
        if (sqlite3_step(stmt) == SQLITE_ROW) s.soft_deleted_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    // Reclaimable bytes: size_bytes of GC-eligible files (soft-deleted, ref_count 0)
    // plus blobs already queued and still pending unlink.
    {
        const char* sql =
            "SELECT COALESCE(SUM(size_bytes), 0) FROM file_locations "
            "WHERE status='deleted' AND ref_count <= 0";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("CX_ERR_GC_INTERNAL: status bytes: ") +
                                    sqlite3_errmsg(db_));
        }
        if (sqlite3_step(stmt) == SQLITE_ROW) s.reclaimable_bytes = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return s;
}

Result<GcRunReport> GcManager::RunOnce() {
    std::lock_guard<std::mutex> lock(mu_);
    return RunLocked(/*bypass_windows=*/false);
}

Result<GcRunReport> GcManager::Purge() {
    std::lock_guard<std::mutex> lock(mu_);
    // Immediate purge: bypass the 30d / 90d windows but keep the per-run caps.
    return RunLocked(/*bypass_windows=*/true);
}

Result<GcRunReport> GcManager::RunLocked(bool bypass_windows) {
    running_ = true;
    const int64_t now = NowMs();
    const int64_t deadline = now + static_cast<int64_t>(config_.max_run_duration_minutes) * 60 * 1000;
    const int cap = config_.max_purge_per_run;

    GcRunReport report;
    Status s2 = Stage2HardDelete(now, bypass_windows, &report.hard_deleted, cap, deadline,
                                 &report.hit_run_cap);
    if (!s2.ok()) {
        running_ = false;
        return s2;
    }
    Status s3 = Stage3BlobGc(now, bypass_windows, &report.blobs_unlinked, cap, deadline,
                             &report.hit_run_cap);
    running_ = false;
    if (!s3.ok()) return s3;

    last_gc_at_ms_ = NowMs();
    return report;
}

// Stage 2: file_locations rows whose soft-delete is older than the retention
// window (or all soft-deleted rows when bypass_windows). For each, recompute
// ref_count from active content_refs; when it is 0, hard-delete the row, drop its
// deleted content_refs, and enqueue its blob_uri for Stage 3.
Status GcManager::Stage2HardDelete(int64_t now_ms, bool bypass_windows, int* out_count,
                                   int cap, int64_t deadline_ms, bool* hit_cap) {
    const int64_t cutoff = bypass_windows
                               ? now_ms  // every soft-deleted row qualifies
                               : now_ms - DaysToMs(config_.soft_delete_retention_days);

    // Collect candidate (file_hash, blob_uri) pairs first (avoids mutating while
    // iterating a live cursor).
    struct Cand {
        std::string file_hash;
        std::string blob_uri;
    };
    std::vector<Cand> candidates;
    {
        // ref_count is recomputed authoritatively from active content_refs (ARCH
        // formula) rather than trusting the cached file_locations.ref_count
        // aggregate — Stage 2 is the point where the spec mandates a recount.
        const char* sql =
            "SELECT fl.file_hash, COALESCE(fl.blob_uri, '') FROM file_locations fl "
            "WHERE fl.status='deleted' AND fl.deleted_at IS NOT NULL AND fl.deleted_at <= ? "
            "AND (SELECT COUNT(*) FROM content_refs cr "
            "     WHERE cr.file_hash=fl.file_hash AND cr.status='active') = 0";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("CX_ERR_GC_INTERNAL: stage2 select: ") +
                                    sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(stmt, 1, cutoff);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* bu = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            candidates.push_back({fh ? fh : "", bu ? bu : ""});
        }
        sqlite3_finalize(stmt);
    }

    int done = 0;
    for (const auto& c : candidates) {
        if (done >= cap || NowMs() >= deadline_ms) {
            if (hit_cap) *hit_cap = true;
            break;
        }
        if (config_.dry_run) {
            ++done;
            continue;
        }
        if (!ExecOk(db_, "BEGIN")) {
            return Status::Internal("CX_ERR_GC_INTERNAL: stage2 begin failed");
        }
        // Enqueue the blob (eligible_at = NOW + blob_gc_retention_days) before the
        // file row is gone. immediate purge collapses the window so Stage 3 in the
        // same sweep can unlink it.
        if (!c.blob_uri.empty()) {
            const int64_t eligible_at =
                bypass_windows ? NowMs() : NowMs() + DaysToMs(config_.blob_gc_retention_days);
            const char* eq =
                "INSERT INTO blob_gc_queue (blob_uri, file_hash, queued_at, eligible_at, "
                "last_ref_check, status) VALUES (?, ?, ?, ?, ?, 'pending') "
                "ON CONFLICT(blob_uri) DO NOTHING";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, eq, -1, &st, nullptr) != SQLITE_OK) {
                ExecOk(db_, "ROLLBACK");
                return Status::Internal(std::string("CX_ERR_GC_INTERNAL: enqueue prepare: ") +
                                        sqlite3_errmsg(db_));
            }
            const int64_t n = NowMs();
            BindText(st, 1, c.blob_uri);
            BindText(st, 2, c.file_hash);
            sqlite3_bind_int64(st, 3, n);
            sqlite3_bind_int64(st, 4, eligible_at);
            sqlite3_bind_int64(st, 5, n);
            const int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                ExecOk(db_, "ROLLBACK");
                return Status::Internal("CX_ERR_GC_INTERNAL: enqueue failed");
            }
        }
        // Drop the soft-deleted content_refs rows for this file (Stage 2 cleanup).
        {
            const char* dr = "DELETE FROM content_refs WHERE file_hash=? AND status='deleted'";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, dr, -1, &st, nullptr) != SQLITE_OK) {
                ExecOk(db_, "ROLLBACK");
                return Status::Internal(std::string("CX_ERR_GC_INTERNAL: cr delete: ") +
                                        sqlite3_errmsg(db_));
            }
            BindText(st, 1, c.file_hash);
            const int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                ExecOk(db_, "ROLLBACK");
                return Status::Internal("CX_ERR_GC_INTERNAL: cr delete failed");
            }
        }
        // Hard-delete the file_locations row.
        {
            const char* df = "DELETE FROM file_locations WHERE file_hash=?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, df, -1, &st, nullptr) != SQLITE_OK) {
                ExecOk(db_, "ROLLBACK");
                return Status::Internal(std::string("CX_ERR_GC_INTERNAL: fl delete: ") +
                                        sqlite3_errmsg(db_));
            }
            BindText(st, 1, c.file_hash);
            const int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                ExecOk(db_, "ROLLBACK");
                return Status::Internal("CX_ERR_GC_INTERNAL: fl delete failed");
            }
        }
        if (!ExecOk(db_, "COMMIT")) {
            ExecOk(db_, "ROLLBACK");
            return Status::Internal("CX_ERR_GC_INTERNAL: stage2 commit failed");
        }
        ++done;
    }
    if (out_count) *out_count = done;
    return Status::Ok();
}

// Stage 3: blob_gc_queue rows past eligible_at (or all pending rows when
// bypass_windows). Unlink the physical blob via the sink, then mark 'unlinked'.
Status GcManager::Stage3BlobGc(int64_t now_ms, bool bypass_windows, int* out_count,
                               int cap, int64_t deadline_ms, bool* hit_cap) {
    std::vector<std::string> uris;
    {
        const char* sql =
            bypass_windows
                ? "SELECT blob_uri FROM blob_gc_queue WHERE status='pending'"
                : "SELECT blob_uri FROM blob_gc_queue WHERE status='pending' AND eligible_at <= ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("CX_ERR_GC_INTERNAL: stage3 select: ") +
                                    sqlite3_errmsg(db_));
        }
        if (!bypass_windows) sqlite3_bind_int64(stmt, 1, now_ms);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* u = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (u) uris.emplace_back(u);
        }
        sqlite3_finalize(stmt);
    }

    int done = 0;
    for (const auto& uri : uris) {
        if (done >= cap || NowMs() >= deadline_ms) {
            if (hit_cap) *hit_cap = true;
            break;
        }
        if (config_.dry_run) {
            ++done;
            continue;
        }
        // Physically delete the blob. A sink error leaves the row 'pending' for the
        // next sweep (no DB mutation), so we surface it but keep going.
        Status us = sink_ ? sink_->Unlink(uri) : Status::Ok();
        if (!us.ok()) continue;

        const char* upd = "UPDATE blob_gc_queue SET status='unlinked' WHERE blob_uri=?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, upd, -1, &st, nullptr) != SQLITE_OK) {
            return Status::Internal(std::string("CX_ERR_GC_INTERNAL: stage3 update: ") +
                                    sqlite3_errmsg(db_));
        }
        BindText(st, 1, uri);
        const int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            return Status::Internal("CX_ERR_GC_INTERNAL: stage3 update failed");
        }
        ++done;
    }
    if (out_count) *out_count = done;
    return Status::Ok();
}

Result<GcManager::RestoreOutcome> GcManager::Restore(const std::vector<std::string>& doc_ids) {
    std::lock_guard<std::mutex> lock(mu_);
    RestoreOutcome out;
    for (const auto& doc_id : doc_ids) {
        // doc_id -> file_hash via content_refs (the file-level GC record).
        std::string file_hash;
        {
            const char* sql = "SELECT file_hash FROM content_refs WHERE doc_id=? LIMIT 1";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
                out.failed.emplace_back(doc_id, "CX_ERR_GC_INTERNAL");
                continue;
            }
            BindText(st, 1, doc_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                if (fh) file_hash = fh;
            }
            sqlite3_finalize(st);
        }
        if (file_hash.empty()) {
            out.failed.emplace_back(doc_id, "CX_ERR_GC_DOC_NOT_FOUND");
            continue;
        }

        if (!ExecOk(db_, "BEGIN")) {
            out.failed.emplace_back(doc_id, "CX_ERR_GC_INTERNAL");
            continue;
        }
        bool ok = true;
        // Reactivate the content_refs rows for this (file_hash, doc_id).
        {
            const char* sql =
                "UPDATE content_refs SET status='active' WHERE file_hash=? AND doc_id=? "
                "AND status='deleted'";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
                BindText(st, 1, file_hash);
                BindText(st, 2, doc_id);
                ok = (sqlite3_step(st) == SQLITE_DONE);
                sqlite3_finalize(st);
            } else {
                ok = false;
            }
        }
        // Clear the Stage-1 soft delete on the file row + recompute ref_count from
        // active content_refs (ARCH status='active' count).
        if (ok) {
            const char* sql =
                "UPDATE file_locations SET status='active', deleted_at=NULL, "
                "ref_count=(SELECT COUNT(*) FROM content_refs "
                "           WHERE file_hash=? AND status='active') "
                "WHERE file_hash=?";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
                BindText(st, 1, file_hash);
                BindText(st, 2, file_hash);
                ok = (sqlite3_step(st) == SQLITE_DONE);
                sqlite3_finalize(st);
            } else {
                ok = false;
            }
        }
        if (ok && ExecOk(db_, "COMMIT")) {
            out.succeeded.push_back(doc_id);
        } else {
            ExecOk(db_, "ROLLBACK");
            out.failed.emplace_back(doc_id, "CX_ERR_GC_INTERNAL");
        }
    }
    return out;
}

Status GcManager::Vacuum() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ExecOk(db_, "VACUUM")) {
        return Status::Internal(std::string("CX_ERR_GC_VACUUM_FAILED: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Status GcManager::Reindex() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ExecOk(db_, "REINDEX")) {
        return Status::Internal(std::string("CX_ERR_GC_REINDEX_FAILED: ") + sqlite3_errmsg(db_));
    }
    return Status::Ok();
}

Status GcManager::EnqueueBlob(const std::string& blob_uri, const std::string& file_hash) {
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t now = NowMs();
    const int64_t eligible_at = now + DaysToMs(config_.blob_gc_retention_days);
    const char* sql =
        "INSERT INTO blob_gc_queue (blob_uri, file_hash, queued_at, eligible_at, "
        "last_ref_check, status) VALUES (?, ?, ?, ?, ?, 'pending') "
        "ON CONFLICT(blob_uri) DO NOTHING";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        return Status::Internal(std::string("CX_ERR_GC_INTERNAL: enqueue prepare: ") +
                                sqlite3_errmsg(db_));
    }
    BindText(st, 1, blob_uri);
    BindText(st, 2, file_hash);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, eligible_at);
    sqlite3_bind_int64(st, 5, now);
    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return Status::Internal("CX_ERR_GC_INTERNAL: enqueue failed");
    }
    return Status::Ok();
}

}  // namespace cortrix::catalog::gc
