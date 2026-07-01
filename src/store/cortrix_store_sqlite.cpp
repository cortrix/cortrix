#include <cstdint>
#include "cortrix/store/cortrix_store_sqlite.h"
#include "cortrix/store/per_unit_schema_ddl.h"  // [D3.5-B] kPerUnitFrameworkDdl (single DDL SoT)
#include "cortrix/store/f34_schema_provider.h"  // [A unified-blocks] standalone child cols + parents
#include "cortrix/logging/logging.h"
#include <sqlite3.h>
#include <algorithm>
#include "cortrix/id/ulid.h"   // id::GenerateUlid (doc_id ULID mint, D-I6)
#include "cortrix/id/types.h"  // id::ToSqliteInt (block_id uint64 → int64, D3.5 wire⑤)

namespace cortrix {

namespace {

const char* kDocStatusToString[] = {"pending", "processing", "ready",
                                    "error", "stale", "deleting", "deleted"};

const char* DocStatusStr(DocStatus s) {
    int idx = static_cast<int>(s);
    if (idx >= 0 && idx <= 6) return kDocStatusToString[idx];
    return "pending";
}

DocStatus DocStatusFromStr(const char* s) {
    if (!s) return DocStatus::kPending;
    if (std::strcmp(s, "processing") == 0) return DocStatus::kProcessing;
    if (std::strcmp(s, "ready") == 0) return DocStatus::kReady;
    if (std::strcmp(s, "error") == 0) return DocStatus::kError;
    if (std::strcmp(s, "stale") == 0) return DocStatus::kStale;
    if (std::strcmp(s, "deleting") == 0) return DocStatus::kDeleting;
    if (std::strcmp(s, "deleted") == 0) return DocStatus::kDeleted;
    return DocStatus::kPending;
}

// Helper to safely get text from sqlite3_column_text (handles NULL)
std::string ColText(sqlite3_stmt* stmt, int col) {
    const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return t ? std::string(t) : std::string();
}

bool ColumnExists(sqlite3* db, const char* table, const char* column) {
    if (db == nullptr) return false;
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool exists = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name != nullptr && std::string(reinterpret_cast<const char*>(name)) == column) {
            exists = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return exists;
}

void ReadDocFromStmt(sqlite3_stmt* stmt, CortrixDoc& doc) {
    doc.doc_id           = ColText(stmt, 0);
    doc.source_type      = ColText(stmt, 1);
    doc.source_path      = ColText(stmt, 2);
    doc.source_ref       = ColText(stmt, 3);
    doc.content_hash     = ColText(stmt, 4);
    doc.file_size        = sqlite3_column_int64(stmt, 5);
    doc.mime_type        = ColText(stmt, 6);
    doc.status           = DocStatusFromStr(ColText(stmt, 7).c_str());
    doc.error_message    = ColText(stmt, 8);
    doc.processing_level = sqlite3_column_int(stmt, 9);
    doc.block_count      = sqlite3_column_int64(stmt, 10);
    doc.chunk_strategy   = ColText(stmt, 11);
    doc.created_at       = ColText(stmt, 12);
    doc.updated_at       = ColText(stmt, 13);
    doc.processed_at     = ColText(stmt, 14);
    doc.cdc_schema       = ColText(stmt, 15);
    doc.cdc_position     = ColText(stmt, 16);
    doc.cdc_last_sync    = ColText(stmt, 17);
    doc.title            = ColText(stmt, 18);
    doc.language         = ColText(stmt, 19);
    doc.metadata_json    = ColText(stmt, 20);
}

}  // namespace

// --- FTS5 Query Sanitization (M-SEC-001) ---

std::string SanitizeFts5Query(const std::string& raw_query) {
    if (raw_query.empty()) return {};

    std::string sanitized;
    sanitized.reserve(raw_query.size() + 32);

    size_t i = 0;
    bool first_token = true;

    while (i < raw_query.size()) {
        // Skip whitespace
        while (i < raw_query.size() && (raw_query[i] == ' ' || raw_query[i] == '\t'
               || raw_query[i] == '\n' || raw_query[i] == '\r')) {
            ++i;
        }
        if (i >= raw_query.size()) break;

        // Extract a token (non-whitespace run)
        size_t start = i;
        while (i < raw_query.size() && raw_query[i] != ' ' && raw_query[i] != '\t'
               && raw_query[i] != '\n' && raw_query[i] != '\r') {
            ++i;
        }

        std::string token = raw_query.substr(start, i - start);

        // Skip tokens that are purely FTS5 syntax characters (* ^ etc.)
        // but keep actual word tokens
        bool has_alnum = false;
        for (char c : token) {
            // Consider UTF-8 multi-byte chars (high bit set) as valid content
            if (std::isalnum(static_cast<unsigned char>(c)) || (static_cast<unsigned char>(c) > 127)) {
                has_alnum = true;
                break;
            }
        }
        if (!has_alnum) continue;

        // Escape any double quotes within the token by doubling them
        std::string escaped;
        escaped.reserve(token.size() + 4);
        for (char c : token) {
            if (c == '"') {
                escaped += '"';  // FTS5 escapes " by doubling it inside quoted string
            }
            escaped += c;
        }

        if (!first_token) {
            sanitized += ' ';
        }
        // Wrap token in double quotes to force literal matching
        sanitized += '"';
        sanitized += escaped;
        sanitized += '"';
        first_token = false;
    }

    return sanitized;
}

CortrixStoreSqlite::CortrixStoreSqlite(const std::string& db_path)
    : db_path_(db_path), owns_db_(true) {}

CortrixStoreSqlite::CortrixStoreSqlite(const std::string& db_path, OpenOptions options)
    : db_path_(db_path), owns_db_(true), opts_(options) {}

// D-I1.bis: borrowed-connection view, single-threaded contexts ONLY (F05
// load-time tooling + tests; see header). We do NOT own the conn — Open()
// skips sqlite3_open, Close()/dtor skip sqlite3_close. db_path_ is a label
// only (for logs). PRAGMAs were already applied by the conn's owner.
CortrixStoreSqlite::CortrixStoreSqlite(sqlite3* external_conn)
    : db_path_("<external-conn>"), db_(external_conn), owns_db_(false) {}

// Testing seam (F23 §4.5, SetFailNextOps): consume one pending fault, if any.
// Production leaves fail_next_ops_ at 0, making this a single relaxed load.
bool CortrixStoreSqlite::TryConsumeOpFault() {
    int pending = fail_next_ops_.load(std::memory_order_relaxed);
    while (pending > 0) {
        if (fail_next_ops_.compare_exchange_weak(pending, pending - 1,
                                                 std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

CortrixStoreSqlite::~CortrixStoreSqlite() {
    Close();
}

int CortrixStoreSqlite::Open() {
    std::lock_guard<std::mutex> lock(mu_);

    if (owns_db_) {
        if (db_) return 0;  // already open (owns mode)

        // Use FULLMUTEX for defense-in-depth: both app-level mu_ and SQLite's
        // internal mutex protect concurrent access.  Avoids heap corruption seen
        // when many CortrixStoreSqlite instances are created/destroyed rapidly
        // in the same process (e.g., unit-test runners).  (SEG-001)
        int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
        if (rc != SQLITE_OK) {
            CORTRIX_LOG_ERROR("store", "Failed to open SQLite: {} ({})", db_path_, sqlite3_errmsg(db_));
            db_ = nullptr;
            return -1;
        }

        if (ExecutePragma() != 0) {
            CORTRIX_LOG_WARN("store", "PRAGMA setup had issues for {}", db_path_);
        }
    } else {
        // D-I1 external-conn mode: the handle was injected at construction and is
        // owned by the F05 SqliteConn (PRAGMAs already applied there, F05 §7.3).
        // A null handle here is a wiring bug.
        if (!db_) {
            CORTRIX_LOG_ERROR("store", "External-conn store Open() with null handle");
            return -1;
        }
    }

    // [D3.5-B] Per-Unit schema: production units are migrated ONCE at F05 load
    // time (F09SchemaProvider via SchemaMigrator::MigrateUnit), so the per-facade
    // production connection passes run_schema_ddl=false (D-I1.bis). The standalone
    // owns-db path has no migrator and still builds the framework schema here
    // (same kPerUnitFrameworkDdl source → byte-identical schema).
    if (owns_db_ && opts_.run_schema_ddl && CreateTables() != 0) {
        CORTRIX_LOG_ERROR("store", "Failed to create tables for {}", db_path_);
        sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }

    // RecoverCrashedDocs (processing→pending reset + delete partial blocks) is a
    // one-shot *startup* recovery — per-request it would be unsafe (a concurrent
    // request mid-processing a doc would be mis-detected as crashed and rolled
    // back). Production runs it ONCE at F05 load time (single-threaded, before
    // any request; namespace_pool assembly step 8b, D-I1.bis), so the per-facade
    // connection passes run_crash_recovery=false. Standalone keeps it in Open().
    if (owns_db_ && opts_.run_crash_recovery) {
        RecoverCrashedDocs();
    }

    return 0;
}

int CortrixStoreSqlite::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (owns_db_ && db_) {
        sqlite3_close(db_);
    }
    // External mode: just drop the borrowed pointer; the F05 SqliteConn owns the
    // handle and closes it when the bundle is released.
    db_ = nullptr;
    return 0;
}

// D3.5 F03/F07: hand back the underlying connection (owned or borrowed) for the
// SPC write path's enrichment persistence. No lock: the caller uses it sequentially
// inside the per-NS F25 write (same discipline as the §9.4 rollback callback, which
// also captures and uses the bare handle); returning the pointer races with nothing.
sqlite3* CortrixStoreSqlite::db_handle() { return db_; }

void CortrixStoreSqlite::EnsureScoreColumnCacheLocked() {
    if (score_columns_checked_) return;
    has_enriched_score_ = ColumnExists(db_, "blocks", "enriched_score");
    has_semantic_score_ = ColumnExists(db_, "blocks", "semantic_score");
    score_columns_checked_ = true;
}

ScoreSignals CortrixStoreSqlite::LoadBlockScoreSignalsLocked(uint64_t block_id) {
    ScoreSignals signals;
    EnsureScoreColumnCacheLocked();
    if (db_ == nullptr || (!has_enriched_score_ && !has_semantic_score_)) return signals;

    std::string cols;
    if (has_enriched_score_) cols += "enriched_score";
    if (has_semantic_score_) {
        if (!cols.empty()) cols += ", ";
        cols += "semantic_score";
    }

    const std::string sql = "SELECT " + cols + " FROM blocks WHERE block_id = ?1 LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return signals;
    }
    sqlite3_bind_int64(stmt, 1, id::ToSqliteInt(block_id));
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int col = 0;
        if (has_enriched_score_) {
            if (sqlite3_column_type(stmt, col) != SQLITE_NULL) {
                signals.enriched_score = static_cast<float>(sqlite3_column_double(stmt, col));
            }
            ++col;
        }
        if (has_semantic_score_) {
            if (sqlite3_column_type(stmt, col) != SQLITE_NULL) {
                signals.semantic_score = static_cast<float>(sqlite3_column_double(stmt, col));
            }
            ++col;
        }
    }
    sqlite3_finalize(stmt);
    return signals;
}

int CortrixStoreSqlite::ExecutePragma() {
    const char* pragmas[] = {
        // busy_timeout FIRST: it only guards statements that run AFTER it. A
        // fresh db's WAL conversion takes an exclusive lock, so concurrent
        // first-opens (per-facade connections, D-I1.bis) would fail immediately
        // with "database is locked" if journal_mode ran before the timeout.
        "PRAGMA busy_timeout = 5000",
        "PRAGMA journal_mode = WAL",
        "PRAGMA synchronous = NORMAL",
        "PRAGMA cache_size = -8000",
        "PRAGMA mmap_size = 268435456",
        "PRAGMA foreign_keys = ON",
        "PRAGMA auto_vacuum = INCREMENTAL",
    };
    for (const char* sql : pragmas) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            CORTRIX_LOG_WARN("store", "PRAGMA failed: {} - {}", sql, err ? err : "unknown");
            sqlite3_free(err);
        }
    }
    return 0;
}

int CortrixStoreSqlite::CreateTables() {
    // [D3.5-B] Single DDL SoT shared with F09SchemaProvider (production MigrateUnit
    // path). This method now serves only the standalone owns-db path (see Open()).
    const char* ddl = store::kPerUnitFrameworkDdl;

    char* err = nullptr;
    int rc = sqlite3_exec(db_, ddl, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "DDL failed: {}", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    // [A unified-blocks] Standalone fidelity: production builds the blocks child
    // columns (child_id/parent_id/token_count/parent_offset) + parents table via
    // F34SchemaProvider in MigrateUnit; the owns-db path has no migrator, so apply
    // the same store-layer provider here so block_insert/block_get (which carry the
    // A columns) match production. F35/F40 columns (contextualized/sparse_vec) are
    // not carried by CortrixBlock/block_insert, so they are intentionally not applied
    // here (added in production MigrateUnit; would be a store→spc/retrieval layering
    // violation to apply from the store).
    store::F34SchemaProvider f34;
    if (Status s = f34.Migrate(db_, 0, 1); !s.ok()) {
        CORTRIX_LOG_ERROR("store", "F34 schema (standalone): {}", s.message());
        return -1;
    }
    return 0;
}

// --- Document CRUD ---

int CortrixStoreSqlite::doc_create(CortrixDoc& doc) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    // D-I6: mint the doc_id ULID at the app layer (no rowid). The INSERT carries
    // doc_id explicitly, appended last so existing bind indices stay unchanged.
    if (doc.doc_id.empty()) doc.doc_id = id::GenerateUlid();

    const char* sql = R"SQL(
        INSERT INTO documents (source_type, source_path, source_ref, content_hash,
            file_size, mime_type, status, error_message, processing_level, block_count,
            chunk_strategy, processed_at, cdc_schema, cdc_position, cdc_last_sync,
            title, language, metadata_json, doc_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_create prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, doc.source_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, doc.source_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, doc.source_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, doc.content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, doc.file_size);
    sqlite3_bind_text(stmt, 6, doc.mime_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, DocStatusStr(doc.status), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, doc.error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, doc.processing_level);
    sqlite3_bind_int64(stmt, 10, doc.block_count);
    sqlite3_bind_text(stmt, 11, doc.chunk_strategy.c_str(), -1, SQLITE_TRANSIENT);
    if (doc.processed_at.empty()) {
        sqlite3_bind_null(stmt, 12);
    } else {
        sqlite3_bind_text(stmt, 12, doc.processed_at.c_str(), -1, SQLITE_TRANSIENT);
    }
    // CDC fields (13-15): bind NULL if empty
    if (doc.cdc_schema.empty()) {
        sqlite3_bind_null(stmt, 13);
    } else {
        sqlite3_bind_text(stmt, 13, doc.cdc_schema.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (doc.cdc_position.empty()) {
        sqlite3_bind_null(stmt, 14);
    } else {
        sqlite3_bind_text(stmt, 14, doc.cdc_position.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (doc.cdc_last_sync.empty()) {
        sqlite3_bind_null(stmt, 15);
    } else {
        sqlite3_bind_text(stmt, 15, doc.cdc_last_sync.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 16, doc.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, doc.language.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 18, doc.metadata_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 19, doc.doc_id.c_str(), -1, SQLITE_TRANSIENT);  // D-I6 ULID

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "doc_create step: {}", sqlite3_errmsg(db_));
        sqlite3_finalize(stmt);
        return -1;
    }

    // doc.doc_id was minted above (TEXT PK — no last_insert_rowid).
    sqlite3_finalize(stmt);

    // Read back created_at and updated_at
    const char* read_sql = "SELECT created_at, updated_at FROM documents WHERE doc_id = ?";
    sqlite3_stmt* read_stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, read_sql, -1, &read_stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(read_stmt, 1, doc.doc_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(read_stmt) == SQLITE_ROW) {
            doc.created_at = ColText(read_stmt, 0);
            doc.updated_at = ColText(read_stmt, 1);
        }
        sqlite3_finalize(read_stmt);
    }

    return 0;
}

int CortrixStoreSqlite::doc_get(const std::string& doc_id, CortrixDoc& doc) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT doc_id, source_type, source_path, source_ref, content_hash,
               file_size, mime_type, status, error_message, processing_level,
               block_count, chunk_strategy, created_at, updated_at, processed_at,
               cdc_schema, cdc_position, cdc_last_sync,
               title, language, metadata_json
        FROM documents WHERE doc_id = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_get prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ReadDocFromStmt(stmt, doc);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -2;  // not found
}

int CortrixStoreSqlite::doc_update_status(const std::string& doc_id, DocStatus status,
                                          const std::string& error_message) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        UPDATE documents SET status = ?, error_message = ?,
               updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        WHERE doc_id = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_update_status prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, DocStatusStr(status), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, doc_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "doc_update_status step: {}", sqlite3_errmsg(db_));
        return -1;
    }

    if (sqlite3_changes(db_) == 0) return -2;  // not found
    return 0;
}

int CortrixStoreSqlite::doc_delete(const std::string& doc_id) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    char* err = nullptr;
    int begin_rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err);
    if (begin_rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_delete BEGIN TRANSACTION failed: {}", err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    sqlite3_free(err);

    // Delete blocks (triggers will clean up FTS5). (R2-M4) Check every rc: a failed
    // prepare/step must ROLLBACK and report failure, not be silently swallowed.
    const char* del_blocks = "DELETE FROM blocks WHERE doc_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, del_blocks, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_delete prepare(blocks) failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step_rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "doc_delete DELETE blocks failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }

    // Delete document
    const char* del_doc = "DELETE FROM documents WHERE doc_id = ?";
    rc = sqlite3_prepare_v2(db_, del_doc, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_delete prepare(document) failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step_rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "doc_delete DELETE document failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }

    int changes = sqlite3_changes(db_);

    // (R2-M4) A failed COMMIT must surface as an error, not a silent success: roll back
    // and return -1 so the caller never treats a partial/lost delete as done.
    err = nullptr;
    int commit_rc = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err);
    if (commit_rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_delete COMMIT failed: {}", err ? err : "unknown");
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }
    sqlite3_free(err);

    return (changes == 0) ? -2 : 0;
}

int CortrixStoreSqlite::doc_delete_by_source_prefix(const std::string& source_path_prefix,
                                                    int64_t* deleted_blocks) {
    if (deleted_blocks) *deleted_blocks = 0;
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    if (source_path_prefix.empty()) return 0;  // refuse to clear the whole NS
    std::lock_guard<std::mutex> lock(mu_);

    // Escape LIKE wildcards in the caller-supplied prefix so a '%' / '_' inside a
    // source URI is matched literally (ESCAPE '\'); then append '%' for the prefix.
    std::string like_pattern;
    like_pattern.reserve(source_path_prefix.size() + 1);
    for (char c : source_path_prefix) {
        if (c == '%' || c == '_' || c == '\\') like_pattern.push_back('\\');
        like_pattern.push_back(c);
    }
    like_pattern.push_back('%');

    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err) != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_delete_by_source_prefix BEGIN failed: {}",
                          err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    sqlite3_free(err);

    // The set of doc_ids to clear (this table's prior import).
    const char* sel_docs =
        "SELECT doc_id FROM documents WHERE source_path LIKE ? ESCAPE '\\'";
    // Count the blocks about to be removed (for the audit / progress record).
    const char* count_blocks =
        "SELECT COUNT(*) FROM blocks WHERE doc_id IN "
        "(SELECT doc_id FROM documents WHERE source_path LIKE ? ESCAPE '\\')";
    // Delete blocks (FTS cleaned by the blocks_ad trigger) then the documents.
    const char* del_blocks =
        "DELETE FROM blocks WHERE doc_id IN "
        "(SELECT doc_id FROM documents WHERE source_path LIKE ? ESCAPE '\\')";
    const char* del_docs =
        "DELETE FROM documents WHERE source_path LIKE ? ESCAPE '\\'";

    auto bind_and_step = [&](const char* sql, int64_t* out_count) -> bool {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(stmt);
        if (out_count && step == SQLITE_ROW) *out_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return true;
    };

    int64_t block_count = 0;
    (void)sel_docs;  // documented set; the count + deletes embed the same subquery
    if (!bind_and_step(count_blocks, &block_count) ||
        !bind_and_step(del_blocks, nullptr) ||
        !bind_and_step(del_docs, nullptr)) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }

    err = nullptr;
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_delete_by_source_prefix COMMIT failed: {}",
                          err ? err : "unknown");
        sqlite3_free(err);
        return -1;
    }
    sqlite3_free(err);
    if (deleted_blocks) *deleted_blocks = block_count;
    return 0;
}

int CortrixStoreSqlite::doc_soft_delete(const std::string& doc_id, int64_t now_ms) {
    // OPEN-2 Stage 1: mark the doc 'deleted' + stamp deleted_at. Idempotent on an
    // already-deleted doc (it stays deleted; deleted_at refreshes are harmless).
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        UPDATE documents SET status = 'deleted', deleted_at = ?,
               updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        WHERE doc_id = ?
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_soft_delete prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, now_ms);
    sqlite3_bind_text(stmt, 2, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "doc_soft_delete step: {}", sqlite3_errmsg(db_));
        return -1;
    }
    return (sqlite3_changes(db_) == 0) ? -2 : 0;
}

int CortrixStoreSqlite::doc_restore(const std::string& doc_id) {
    // OPEN-2 Stage 1 reversal: clear deleted_at + flip 'deleted' back to 'ready'.
    // Guarded on status='deleted' so it only restores a soft-deleted doc (a live
    // doc is untouched → -2 "nothing to restore").
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        UPDATE documents SET status = 'ready', deleted_at = NULL,
               updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now')
        WHERE doc_id = ? AND status = 'deleted'
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "doc_restore prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "doc_restore step: {}", sqlite3_errmsg(db_));
        return -1;
    }
    return (sqlite3_changes(db_) == 0) ? -2 : 0;
}

int CortrixStoreSqlite::doc_list_deleted_before(int64_t cutoff_ms,
                                                std::vector<CortrixDoc>& out) {
    // OPEN-2 Stage 2 candidates: soft-deleted docs whose retention window has
    // elapsed (deleted_at <= cutoff). cutoff = now - soft_delete_retention_days;
    // an immediate-purge sweep passes a future cutoff to catch every deleted doc.
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT doc_id, source_type, source_path, source_ref, content_hash,
               file_size, mime_type, status, error_message, processing_level,
               block_count, chunk_strategy, created_at, updated_at, processed_at,
               cdc_schema, cdc_position, cdc_last_sync,
               title, language, metadata_json
        FROM documents
        WHERE status = 'deleted' AND deleted_at IS NOT NULL AND deleted_at <= ?
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, cutoff_ms);
    out.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CortrixDoc doc;
        ReadDocFromStmt(stmt, doc);
        out.push_back(std::move(doc));
    }
    sqlite3_finalize(stmt);
    return 0;
}

int CortrixStoreSqlite::doc_list_by_status(DocStatus status,
                                            std::vector<CortrixDoc>& out) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT doc_id, source_type, source_path, source_ref, content_hash,
               file_size, mime_type, status, error_message, processing_level,
               block_count, chunk_strategy, created_at, updated_at, processed_at,
               cdc_schema, cdc_position, cdc_last_sync,
               title, language, metadata_json
        FROM documents WHERE status = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, DocStatusStr(status), -1, SQLITE_STATIC);

    out.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CortrixDoc doc;
        ReadDocFromStmt(stmt, doc);
        out.push_back(std::move(doc));
    }

    sqlite3_finalize(stmt);
    return 0;
}

int CortrixStoreSqlite::doc_find_by_source(const std::string& source_type,
                                            const std::string& source_path,
                                            CortrixDoc& doc) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT doc_id, source_type, source_path, source_ref, content_hash,
               file_size, mime_type, status, error_message, processing_level,
               block_count, chunk_strategy, created_at, updated_at, processed_at,
               cdc_schema, cdc_position, cdc_last_sync,
               title, language, metadata_json
        FROM documents WHERE source_type = ? AND source_path = ? LIMIT 1
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, source_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, source_path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ReadDocFromStmt(stmt, doc);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -2;
}

int CortrixStoreSqlite::doc_find_by_hash(const std::string& content_hash,
                                          CortrixDoc& doc) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    if (content_hash.empty()) return -2;

    const char* sql = R"SQL(
        SELECT doc_id, source_type, source_path, source_ref, content_hash,
               file_size, mime_type, status, error_message, processing_level,
               block_count, chunk_strategy, created_at, updated_at, processed_at,
               cdc_schema, cdc_position, cdc_last_sync,
               title, language, metadata_json
        FROM documents WHERE content_hash = ? LIMIT 1
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        ReadDocFromStmt(stmt, doc);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -2;
}

int CortrixStoreSqlite::doc_count(int64_t* count) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = "SELECT COUNT(*) FROM documents";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

// --- Block CRUD ---

int CortrixStoreSqlite::block_insert(CortrixBlock& block) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    // D3.5 wire⑤: the business layer (BlockAssembler / SPC) provides a real uint64
    // block_id = HashChildIdToBlockId(child_id). When non-zero we INSERT it
    // explicitly; a zero id (test / legacy callers) falls back to the SQLite rowid,
    // mirroring the dual-mode store ctor (D-I1). uint64 ↔ int64 is bit-preserving
    // (id::ToSqliteInt), matching the FTS5 content_rowid='block_id' triggers.
    const bool explicit_id = (block.block_id != 0);

    // [A unified-blocks] +5 columns: child_id/parent_id/token_count/parent_offset
    // (F34, NULL for non-child rows) + metadata_json (F09-framework shared JSONB).
    const char* sql = explicit_id ? R"SQL(
        INSERT INTO blocks (block_id, doc_id, chunk_index, block_type, processing_level,
                            hnsw_node_id, content_hash, data, content_text,
                            child_id, parent_id, token_count, parent_offset, metadata_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL" : R"SQL(
        INSERT INTO blocks (doc_id, chunk_index, block_type, processing_level,
                            hnsw_node_id, content_hash, data, content_text,
                            child_id, parent_id, token_count, parent_offset, metadata_json)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "block_insert prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }

    int col = 1;
    if (explicit_id) {
        sqlite3_bind_int64(stmt, col++, id::ToSqliteInt(block.block_id));
    }
    sqlite3_bind_text(stmt, col++, block.doc_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, col++, block.chunk_index);
    sqlite3_bind_int(stmt, col++, block.block_type);
    sqlite3_bind_int(stmt, col++, block.processing_level);
    if (block.hnsw_node_id >= 0) {
        sqlite3_bind_int64(stmt, col++, block.hnsw_node_id);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    if (!block.content_hash.empty()) {
        // Design D2: content_hash is BLOB (SHA-256 first 16 bytes)
        sqlite3_bind_blob(stmt, col++, block.content_hash.data(),
                          static_cast<int>(block.content_hash.size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    sqlite3_bind_blob(stmt, col++, block.data.data(),
                      static_cast<int>(block.data.size()), SQLITE_TRANSIENT);
    if (!block.content_text.empty()) {
        sqlite3_bind_text(stmt, col++, block.content_text.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    // [A unified-blocks] child_id / parent_id / token_count / parent_offset / metadata_json.
    // Empty string / -1 sentinel → SQL NULL (a non-child block leaves these unset).
    if (!block.child_id.empty()) {
        sqlite3_bind_text(stmt, col++, block.child_id.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    if (!block.parent_id.empty()) {
        sqlite3_bind_text(stmt, col++, block.parent_id.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    if (block.token_count >= 0) {
        sqlite3_bind_int64(stmt, col++, block.token_count);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    if (block.parent_offset >= 0) {
        sqlite3_bind_int64(stmt, col++, block.parent_offset);
    } else {
        sqlite3_bind_null(stmt, col++);
    }
    if (!block.metadata_json.empty()) {
        sqlite3_bind_text(stmt, col++, block.metadata_json.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, col++);
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        CORTRIX_LOG_ERROR("store", "block_insert step: {}", sqlite3_errmsg(db_));
        sqlite3_finalize(stmt);
        return -1;
    }

    if (!explicit_id) {
        block.block_id = sqlite3_last_insert_rowid(db_);  // rowid fallback (test/legacy)
    }
    sqlite3_finalize(stmt);
    return 0;
}

int CortrixStoreSqlite::block_get(uint64_t block_id, CortrixBlock& block) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT block_id, doc_id, chunk_index, block_type, processing_level,
               hnsw_node_id, content_hash, data, content_text,
               child_id, parent_id, token_count, parent_offset, metadata_json
        FROM blocks WHERE block_id = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, block_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        block.block_id = sqlite3_column_int64(stmt, 0);
        block.doc_id = ColText(stmt, 1);
        block.chunk_index = sqlite3_column_int(stmt, 2);
        block.block_type = sqlite3_column_int(stmt, 3);
        block.processing_level = sqlite3_column_int(stmt, 4);
        block.hnsw_node_id = sqlite3_column_type(stmt, 5) == SQLITE_NULL
                                 ? -1
                                 : sqlite3_column_int64(stmt, 5);
        // content_hash stored as BLOB (SHA-256 first 16 bytes)
        {
            const void* hash_blob = sqlite3_column_blob(stmt, 6);
            int hash_len = sqlite3_column_bytes(stmt, 6);
            if (hash_blob && hash_len > 0) {
                block.content_hash.assign(reinterpret_cast<const char*>(hash_blob), hash_len);
            }
        }

        const void* blob = sqlite3_column_blob(stmt, 7);
        int blob_len = sqlite3_column_bytes(stmt, 7);
        if (blob && blob_len > 0) {
            block.data.assign(reinterpret_cast<const uint8_t*>(blob),
                              reinterpret_cast<const uint8_t*>(blob) + blob_len);
        }
        block.content_text = ColText(stmt, 8);
        // [A unified-blocks] child_id / parent_id / token_count / parent_offset / metadata_json.
        block.child_id = ColText(stmt, 9);
        block.parent_id = ColText(stmt, 10);
        block.token_count = sqlite3_column_type(stmt, 11) == SQLITE_NULL
                                ? -1 : sqlite3_column_int64(stmt, 11);
        block.parent_offset = sqlite3_column_type(stmt, 12) == SQLITE_NULL
                                  ? -1 : sqlite3_column_int64(stmt, 12);
        block.metadata_json = ColText(stmt, 13);

        sqlite3_finalize(stmt);
        block.score_signals = LoadBlockScoreSignalsLocked(block.block_id);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -2;
}

int CortrixStoreSqlite::block_get_by_doc(const std::string& doc_id,
                                          std::vector<CortrixBlock>& out) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT block_id, doc_id, chunk_index, block_type, processing_level,
               hnsw_node_id, content_hash, data, content_text,
               child_id, parent_id, token_count, parent_offset, metadata_json
        FROM blocks WHERE doc_id = ? ORDER BY chunk_index ASC
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);

    out.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CortrixBlock b;
        b.block_id = sqlite3_column_int64(stmt, 0);
        b.doc_id = ColText(stmt, 1);
        b.chunk_index = sqlite3_column_int(stmt, 2);
        b.block_type = sqlite3_column_int(stmt, 3);
        b.processing_level = sqlite3_column_int(stmt, 4);
        b.hnsw_node_id = sqlite3_column_type(stmt, 5) == SQLITE_NULL
                              ? -1
                              : sqlite3_column_int64(stmt, 5);
        // content_hash stored as BLOB
        {
            const void* hash_blob = sqlite3_column_blob(stmt, 6);
            int hash_len = sqlite3_column_bytes(stmt, 6);
            if (hash_blob && hash_len > 0) {
                b.content_hash.assign(reinterpret_cast<const char*>(hash_blob), hash_len);
            }
        }

        const void* blob = sqlite3_column_blob(stmt, 7);
        int blob_len = sqlite3_column_bytes(stmt, 7);
        if (blob && blob_len > 0) {
            b.data.assign(reinterpret_cast<const uint8_t*>(blob),
                          reinterpret_cast<const uint8_t*>(blob) + blob_len);
        }
        b.content_text = ColText(stmt, 8);
        // [A unified-blocks] child_id / parent_id / token_count / parent_offset / metadata_json.
        b.child_id = ColText(stmt, 9);
        b.parent_id = ColText(stmt, 10);
        b.token_count = sqlite3_column_type(stmt, 11) == SQLITE_NULL
                            ? -1 : sqlite3_column_int64(stmt, 11);
        b.parent_offset = sqlite3_column_type(stmt, 12) == SQLITE_NULL
                              ? -1 : sqlite3_column_int64(stmt, 12);
        b.metadata_json = ColText(stmt, 13);

        out.push_back(std::move(b));
    }

    sqlite3_finalize(stmt);
    for (auto& b : out) {
        b.score_signals = LoadBlockScoreSignalsLocked(b.block_id);
    }
    return 0;
}

int CortrixStoreSqlite::block_delete_by_doc(const std::string& doc_id) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = "DELETE FROM blocks WHERE doc_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int CortrixStoreSqlite::block_count(int64_t* count) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = "SELECT COUNT(*) FROM blocks";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

int CortrixStoreSqlite::block_count_by_doc(const std::string& doc_id, int64_t* count) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = "SELECT COUNT(*) FROM blocks WHERE doc_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, doc_id.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

// --- Parent CRUD (F34 `parents` table; A unified-blocks) ---

int CortrixStoreSqlite::parent_insert(CortrixParent& parent) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    // The `parents` table is created standalone by F34SchemaProvider in CreateTables()
    // and in production by F34SchemaProvider@MigrateUnit, so its 11 written columns
    // exist on both paths. The 3 D8 hotness columns are left to their DDL DEFAULTs
    // (V1.0 does not write them). No BEGIN/COMMIT here: a single INSERT autocommits
    // standalone, and the F25 PWL wraps it in the SPC write transaction (ARCH §3.2).
    const char* sql = R"SQL(
        INSERT INTO parents (parent_id, doc_id, namespace_id, parent_text, token_count,
                             page_start, page_end, byte_offset_start, byte_offset_end,
                             metadata_json, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "parent_insert prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }

    int col = 1;
    sqlite3_bind_text(stmt, col++, parent.parent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, col++, parent.doc_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, col++, parent.namespace_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, col++, parent.parent_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, col++, parent.token_count);
    sqlite3_bind_int64(stmt, col++, parent.page_start);
    sqlite3_bind_int64(stmt, col++, parent.page_end);
    sqlite3_bind_int64(stmt, col++, parent.byte_offset_start);
    sqlite3_bind_int64(stmt, col++, parent.byte_offset_end);
    // metadata_json is NOT NULL in the DDL → always bind text (empty string allowed).
    sqlite3_bind_text(stmt, col++, parent.metadata_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, col++, parent.created_at);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        // UNIQUE/PK(parent_id) violation → already exists (-3), else generic error.
        int ext = sqlite3_extended_errcode(db_);
        CORTRIX_LOG_ERROR("store", "parent_insert step: {}", sqlite3_errmsg(db_));
        sqlite3_finalize(stmt);
        return (ext == SQLITE_CONSTRAINT_PRIMARYKEY || ext == SQLITE_CONSTRAINT_UNIQUE)
                   ? -3 : -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int CortrixStoreSqlite::parent_get(const std::string& parent_id, CortrixParent& out) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = R"SQL(
        SELECT parent_id, doc_id, namespace_id, parent_text, token_count,
               page_start, page_end, byte_offset_start, byte_offset_end,
               metadata_json, created_at
        FROM parents WHERE parent_id = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, parent_id.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out.parent_id         = ColText(stmt, 0);
        out.doc_id            = ColText(stmt, 1);
        out.namespace_id      = ColText(stmt, 2);
        out.parent_text       = ColText(stmt, 3);
        out.token_count       = sqlite3_column_int64(stmt, 4);
        out.page_start        = sqlite3_column_int64(stmt, 5);
        out.page_end          = sqlite3_column_int64(stmt, 6);
        out.byte_offset_start = sqlite3_column_int64(stmt, 7);
        out.byte_offset_end   = sqlite3_column_int64(stmt, 8);
        out.metadata_json     = ColText(stmt, 9);
        out.created_at        = sqlite3_column_int64(stmt, 10);
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -2;
}

// --- FTS5 Search ---

int CortrixStoreSqlite::search_fulltext(const std::string& query, int top_k,
                                         std::vector<SearchResult>& results) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    std::lock_guard<std::mutex> lock(mu_);

    // Sanitize query to prevent FTS5 operator injection (M-SEC-001)
    std::string sanitized = SanitizeFts5Query(query);
    if (sanitized.empty()) {
        results.clear();
        return 0;  // empty/whitespace-only query returns no results
    }

    // [OPEN-2] Join documents to exclude blocks of soft-deleted docs (status=
    // 'deleted'): a soft-deleted doc is invisible to retrieval at the SQL level,
    // so BM25/FTS never surfaces it (the dense path filters in PostFilter).
    const char* sql = R"SQL(
        SELECT b.block_id, b.doc_id, rank, b.content_text
        FROM blocks_fts
        JOIN blocks b ON b.block_id = blocks_fts.rowid
        JOIN documents d ON d.doc_id = b.doc_id
        WHERE blocks_fts MATCH ? AND d.status != 'deleted'
        ORDER BY rank
        LIMIT ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        CORTRIX_LOG_ERROR("store", "search_fulltext prepare: {}", sqlite3_errmsg(db_));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, sanitized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, top_k);

    results.clear();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SearchResult r;
        r.block_id = sqlite3_column_int64(stmt, 0);
        r.doc_id = ColText(stmt, 1);
        r.score = sqlite3_column_double(stmt, 2);
        r.content_text = ColText(stmt, 3);
        results.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    return 0;
}

int CortrixStoreSqlite::search_metadata(const std::string& /*filter*/, int /*top_k*/,
                                         std::vector<SearchResult>& /*results*/) {
    if (TryConsumeOpFault()) return -1;  // testing seam (F23 §4.5)
    return -1;  // MVP: not implemented
}

// --- Crash Recovery ---

int CortrixStoreSqlite::RecoverCrashedDocs() {
    // Phase 1: Find docs stuck in 'processing' → clean blocks, reset to pending
    const char* find_processing_sql =
        "SELECT doc_id FROM documents WHERE status = 'processing'";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, find_processing_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    std::vector<std::string> processing_ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        processing_ids.push_back(ColText(stmt, 0));
    }
    sqlite3_finalize(stmt);

    // Delete residual blocks for processing docs and reset status to pending
    for (const std::string& id : processing_ids) {
        const char* del_sql = "DELETE FROM blocks WHERE doc_id = ?";
        sqlite3_stmt* del_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, del_sql, -1, &del_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
    }

    if (!processing_ids.empty()) {
        const char* reset_sql =
            "UPDATE documents SET status = 'pending', "
            "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
            "WHERE status = 'processing'";
        char* err = nullptr;
        sqlite3_exec(db_, reset_sql, nullptr, nullptr, &err);
        if (err) { sqlite3_free(err); }
    }

    // Phase 2: Find docs stuck in 'deleting' → complete the cascade delete
    // Per D5 design: if crash during deletion, complete the delete on recovery
    const char* find_deleting_sql =
        "SELECT doc_id FROM documents WHERE status = 'deleting'";
    stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, find_deleting_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    std::vector<std::string> deleting_ids;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        deleting_ids.push_back(ColText(stmt, 0));
    }
    sqlite3_finalize(stmt);

    // Complete cascade delete for docs that were mid-deletion
    for (const std::string& id : deleting_ids) {
        // Delete blocks first
        const char* del_blocks_sql = "DELETE FROM blocks WHERE doc_id = ?";
        sqlite3_stmt* del_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, del_blocks_sql, -1, &del_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }

        // Delete the document itself
        const char* del_doc_sql = "DELETE FROM documents WHERE doc_id = ?";
        del_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, del_doc_sql, -1, &del_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(del_stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(del_stmt);
            sqlite3_finalize(del_stmt);
        }
    }

    int total_recovered = static_cast<int>(processing_ids.size() + deleting_ids.size());
    if (total_recovered > 0) {
        CORTRIX_LOG_WARN("store", "Recovered {} crashed documents ({} processing, {} deleting)",
                        total_recovered, processing_ids.size(), deleting_ids.size());
    }
    return 0;
}

}  // namespace cortrix
