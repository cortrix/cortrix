#include <cstdint>
#include "cortrix/memory/memory_store.h"
#include <sqlite3.h>
#include <random>
#include <chrono>
#include <cstring>
#include <spdlog/spdlog.h>
#include "cortrix/agent_trace/interaction_sources_schema.h"   // InteractionSourcesSchemaProvider (per-NS; agent_trace is global)

namespace cortrix {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int ExecSQL(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        spdlog::error("SQL exec failed: {} — {}", err ? err : "unknown", sql);
        sqlite3_free(err);
    }
    return rc;
}

/// Read one InteractionLog row from a prepared statement at current position.
/// Expects columns in order: id, session_id, namespace_name, user_id, role,
/// content, query_type, status, latency_ms, metadata_json, created_at
static std::string SafeColText(sqlite3_stmt* stmt, int col) {
    auto t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : "";
}

static InteractionLog ReadInteractionRow(sqlite3_stmt* stmt) {
    InteractionLog log;
    log.id              = SafeColText(stmt, 0);
    log.session_id      = SafeColText(stmt, 1);
    log.namespace_name  = SafeColText(stmt, 2);
    log.user_id         = SafeColText(stmt, 3);
    log.role            = SafeColText(stmt, 4);
    log.content         = SafeColText(stmt, 5);
    log.query_type      = SafeColText(stmt, 6);
    log.status          = SafeColText(stmt, 7);
    log.latency_ms      = sqlite3_column_int(stmt, 8);
    log.metadata_json   = SafeColText(stmt, 9);
    log.created_at      = SafeColText(stmt, 10);
    return log;
}

/// Escape SQL LIKE wildcards (% and _) in user input, using \ as escape char.
static std::string EscapeLikePattern(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size());
    for (char c : input) {
        if (c == '%' || c == '_' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

/// Whether `table` already has a column named `column` (via pragma_table_info).
/// SQLite ADD COLUMN is not "if not exists", so the opt-out migration gates on this —
/// same idiom as src/spc/*_schema_provider.cpp.
static bool ColumnExists(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM pragma_table_info(?1) WHERE name=?2";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, column, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

MemoryStore::MemoryStore(CortrixStore& store) : store_(store) {}

MemoryStore::~MemoryStore() {
    if (db_ && owns_db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

Status MemoryStore::Init(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mu_);

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return Status::Internal("Failed to open memory DB: " + msg);
    }
    owns_db_ = true;

    // busy_timeout FIRST: it only guards statements that run AFTER it. The
    // per-facade memory db is opened on every pool Acquire; concurrent first-init
    // of the same namespace's memory.db races on the WAL conversion's exclusive
    // lock (and CREATE TABLE), so the timeout must already be set when
    // journal_mode runs — otherwise the loser fails immediately with
    // "database is locked" instead of queueing.
    ExecSQL(db_, "PRAGMA busy_timeout=5000;");
    // WAL mode for concurrency
    ExecSQL(db_, "PRAGMA journal_mode=WAL;");
    ExecSQL(db_, "PRAGMA foreign_keys=ON;");

    auto s = CreateMemorySessionsTable();
    if (!s.ok()) return s;

    s = CreateInteractionLogTable();
    if (!s.ok()) return s;

    // Add the opt-out columns + partial index. Idempotent (ADD COLUMN guarded
    // on pragma_table_info), so it runs on both fresh and pre-existing DBs — fresh
    // DBs get the columns here since the base CREATE TABLE above does not list them
    // (keeps the MVP table definition frozen).
    s = MigrateMem04OptOutColumns();
    if (!s.ok()) return s;

    // interaction_sources lives in this memory.db (its FK references the
    // per-NS interaction_log.id, so it must share that db). agent_trace does NOT —
    // TC4 moved it back to the global cortrix_global.db, where GET /traces
    // can read a session whose calls span namespaces. Idempotent; failure here is
    // non-fatal to the memory path — source attribution degrades but
    // sessions/interactions keep working.
    if (Status f13 = CreateF13ObservabilityTables(); !f13.ok()) {
        // log-and-continue: do not block memory.db init on the observability tables.
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// DDL
// ---------------------------------------------------------------------------

Status MemoryStore::CreateF13ObservabilityTables() {
    if (!db_) return Status::InvalidArgument("CreateF13ObservabilityTables: null db");
    // Only interaction_sources is per-NS here: its FK references this db's
    // interaction_log.id (frozen), so the two must co-locate. agent_trace is
    // global (created against cortrix_global.db at startup) — it is NOT
    // created here any more. The provider is idempotent (CREATE TABLE IF NOT EXISTS),
    // so re-running on an existing db is a no-op.
    agent_trace::InteractionSourcesSchemaProvider sources_provider;
    if (Status s = sources_provider.Migrate(db_, 0, sources_provider.CurrentVersion());
        !s.ok()) {
        return s;
    }
    return Status::Ok();
}

Status MemoryStore::CreateInteractionLogTable() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS interaction_log (
            id              TEXT PRIMARY KEY,
            session_id      TEXT NOT NULL,
            namespace_name  TEXT NOT NULL,
            user_id         TEXT,
            role            TEXT NOT NULL,
            content         TEXT NOT NULL,
            query_type      TEXT,
            status          TEXT,
            latency_ms      INTEGER DEFAULT 0,
            metadata_json   TEXT,
            created_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
        );
        CREATE INDEX IF NOT EXISTS idx_interaction_session
            ON interaction_log(session_id, created_at);
        CREATE INDEX IF NOT EXISTS idx_interaction_ns_time
            ON interaction_log(namespace_name, created_at);
        CREATE INDEX IF NOT EXISTS idx_interaction_user
            ON interaction_log(user_id, created_at)
            WHERE user_id IS NOT NULL;
    )";
    if (ExecSQL(db_, sql) != SQLITE_OK) {
        return Status::Internal("Failed to create interaction_log table");
    }
    return Status::Ok();
}

Status MemoryStore::CreateMemorySessionsTable() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS memory_sessions (
            session_id        TEXT PRIMARY KEY,
            namespace_name    TEXT NOT NULL,
            user_id           TEXT,
            title             TEXT,
            interaction_count INTEGER DEFAULT 0,
            doc_id            TEXT,
            created_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
            updated_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
        );
        CREATE INDEX IF NOT EXISTS idx_session_ns
            ON memory_sessions(namespace_name, updated_at DESC);
        CREATE INDEX IF NOT EXISTS idx_session_user
            ON memory_sessions(user_id, updated_at DESC)
            WHERE user_id IS NOT NULL;
    )";
    if (ExecSQL(db_, sql) != SQLITE_OK) {
        return Status::Internal("Failed to create memory_sessions table");
    }
    return Status::Ok();
}

Status MemoryStore::MigrateMem04OptOutColumns() {
    // ColumnExists + ADD COLUMN is check-then-act: two connections first-initing
    // the same db can both see "missing" and both ALTER — the loser gets
    // "duplicate column name", which means the WINNER already reached the target
    // state. Re-check on failure and treat a now-present column as success
    // (assembly-time pre-warm makes this rare; this keeps bare concurrent Init
    // robust too).
    auto add_column_if_missing = [this](const char* table, const char* column,
                                        const char* alter_sql) -> bool {
        if (ColumnExists(db_, table, column)) return true;
        if (ExecSQL(db_, alter_sql) == SQLITE_OK) return true;
        return ColumnExists(db_, table, column);  // lost a benign race → fine
    };

    // memory_sessions += opt_out_at / opted_out_by (opt-out migration). NULL = active.
    if (!add_column_if_missing(
            "memory_sessions", "opt_out_at",
            "ALTER TABLE memory_sessions ADD COLUMN opt_out_at TEXT DEFAULT NULL")) {
        return Status::Internal("MEM04: add memory_sessions.opt_out_at failed");
    }
    if (!add_column_if_missing(
            "memory_sessions", "opted_out_by",
            "ALTER TABLE memory_sessions ADD COLUMN opted_out_by TEXT DEFAULT NULL")) {
        return Status::Internal("MEM04: add memory_sessions.opted_out_by failed");
    }
    // Partial index on opted-out sessions only (keeps the
    // is_session_opted_out + opted-out enumeration cheap without bloating the index
    // with the common active rows).
    if (ExecSQL(db_,
            "CREATE INDEX IF NOT EXISTS idx_memory_sessions_opt_out_at "
            "ON memory_sessions(opt_out_at) WHERE opt_out_at IS NOT NULL")
        != SQLITE_OK) {
        return Status::Internal("MEM04: index memory_sessions.opt_out_at failed");
    }

    // interaction_log += remember (opt-out migration). DEFAULT TRUE: existing rows and
    // new interactions are remembered unless explicitly opted out.
    if (!add_column_if_missing(
            "interaction_log", "remember",
            "ALTER TABLE interaction_log ADD COLUMN remember BOOLEAN DEFAULT 1")) {
        return Status::Internal("MEM04: add interaction_log.remember failed");
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// UUID v4 generation
// ---------------------------------------------------------------------------

std::string MemoryStore::GenerateUUID() {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    uint32_t a = dist(gen);
    uint32_t b = dist(gen);
    uint32_t c = dist(gen);
    uint32_t d = dist(gen);

    // Set version 4 (bits 12-15 of time_hi = 0100)
    b = (b & 0xFFFF0FFF) | 0x00004000;
    // Set variant (bits 6-7 of clock_seq_hi = 10)
    c = (c & 0x3FFFFFFF) | 0x80000000;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%04x%08x",
        a,
        (b >> 16) & 0xFFFF,
        b & 0xFFFF,
        (c >> 16) & 0xFFFF,
        c & 0xFFFF,
        d);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Time helper
// ---------------------------------------------------------------------------

std::string MemoryStore::NowISO8601() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    gmtime_r(&time_t_now, &tm_buf);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms.count()));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Session CRUD
// ---------------------------------------------------------------------------


// Testing seam (SetFailNextOps): consume one pending fault, if any.
// Production leaves fail_next_ops_ at 0, making this a single relaxed load.
bool MemoryStore::TryConsumeOpFault() {
    int pending = fail_next_ops_.load(std::memory_order_relaxed);
    while (pending > 0) {
        if (fail_next_ops_.compare_exchange_weak(pending, pending - 1,
                                                 std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

Status MemoryStore::SessionCreate(MemorySession& session) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    // Accept client-provided session_id if non-empty; otherwise generate one
    if (session.session_id.empty()) {
        session.session_id = GenerateUUID();
    }
    std::string now = NowISO8601();
    session.created_at = now;
    session.updated_at = now;
    session.interaction_count = 0;

    const char* sql =
        "INSERT INTO memory_sessions "
        "(session_id, namespace_name, user_id, title, interaction_count, doc_id, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, 0, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionCreate prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session.namespace_name.c_str(), -1, SQLITE_TRANSIENT);
    if (session.user_id.empty()) {
        sqlite3_bind_null(stmt, 3);
    } else {
        sqlite3_bind_text(stmt, 3, session.user_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (session.title.empty()) {
        sqlite3_bind_null(stmt, 4);
    } else {
        sqlite3_bind_text(stmt, 4, session.title.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!session.doc_id.empty()) {
        sqlite3_bind_text(stmt, 5, session.doc_id.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    sqlite3_bind_text(stmt, 6, session.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, session.updated_at.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Status::Internal("SessionCreate insert: " + std::string(sqlite3_errmsg(db_)));
    }
    return Status::Ok();
}

Status MemoryStore::SessionGet(const std::string& session_id, MemorySession& session) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    // opt_out_at / opted_out_by appended last so the frozen column
    // indices above are unchanged; NULL → empty string (active session).
    const char* sql =
        "SELECT session_id, namespace_name, user_id, title, interaction_count, "
        "doc_id, created_at, updated_at, opt_out_at, opted_out_by "
        "FROM memory_sessions WHERE session_id = ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionGet prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        session.session_id      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        session.namespace_name  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto col2 = sqlite3_column_text(stmt, 2);
        session.user_id = col2 ? reinterpret_cast<const char*>(col2) : "";
        auto col3 = sqlite3_column_text(stmt, 3);
        session.title = col3 ? reinterpret_cast<const char*>(col3) : "";
        session.interaction_count = sqlite3_column_int(stmt, 4);
        session.doc_id = SafeColText(stmt, 5);
        session.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        session.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        session.opt_out_at = SafeColText(stmt, 8);
        session.opted_out_by = SafeColText(stmt, 9);
        sqlite3_finalize(stmt);
        return Status::Ok();
    }

    sqlite3_finalize(stmt);
    return Status::NotFound("Session not found: " + session_id);
}

Status MemoryStore::SessionList(const std::string& namespace_name,
                                int limit, int offset,
                                std::vector<MemorySession>& sessions,
                                const std::string& user_id) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    // When a user_id is supplied, push the per-user predicate into SQL so
    // LIMIT/OFFSET paginate over the owner's sessions (a post-filter on an
    // NS-wide page breaks pagination — total_count/has_more diverge from the
    // returned rows). The user_id predicate sits before ORDER/LIMIT/OFFSET.
    const std::string sql =
        std::string("SELECT session_id, namespace_name, user_id, title, interaction_count, "
                    "doc_id, created_at, updated_at FROM memory_sessions "
                    "WHERE namespace_name = ?") +
        (user_id.empty() ? "" : " AND user_id = ?") +
        " ORDER BY updated_at DESC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionList prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    int bind_idx = 1;
    sqlite3_bind_text(stmt, bind_idx++, namespace_name.c_str(), -1, SQLITE_TRANSIENT);
    if (!user_id.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, user_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bind_idx++, limit);
    sqlite3_bind_int(stmt, bind_idx++, offset);

    sessions.clear();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        MemorySession s;
        s.session_id      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        s.namespace_name  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        auto col2 = sqlite3_column_text(stmt, 2);
        s.user_id = col2 ? reinterpret_cast<const char*>(col2) : "";
        auto col3 = sqlite3_column_text(stmt, 3);
        s.title = col3 ? reinterpret_cast<const char*>(col3) : "";
        s.interaction_count = sqlite3_column_int(stmt, 4);
        s.doc_id = SafeColText(stmt, 5);
        s.created_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        s.updated_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        sessions.push_back(std::move(s));
    }

    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::SessionDelete(const std::string& session_id) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    // Check session exists
    {
        const char* check_sql =
            "SELECT session_id FROM memory_sessions WHERE session_id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW) {
            return Status::NotFound("Session not found: " + session_id);
        }
    }

    // Cascade delete within transaction
    ExecSQL(db_, "BEGIN TRANSACTION");

    // Delete interactions
    {
        const char* sql = "DELETE FROM interaction_log WHERE session_id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Delete session
    {
        const char* sql = "DELETE FROM memory_sessions WHERE session_id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            ExecSQL(db_, "ROLLBACK");
            return Status::Internal("SessionDelete failed: " + std::string(sqlite3_errmsg(db_)));
        }
    }

    ExecSQL(db_, "COMMIT");
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Interaction CRUD
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Unlocked helpers (mu_ must be held by caller)
// ---------------------------------------------------------------------------

Status MemoryStore::InsertInteractionLocked(InteractionLog& log) {
    log.id = GenerateUUID();
    if (log.created_at.empty()) {
        log.created_at = NowISO8601();
    }

    const char* sql =
        "INSERT INTO interaction_log "
        "(id, session_id, namespace_name, user_id, role, content, "
        "query_type, status, latency_ms, metadata_json, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("InteractionInsert prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, log.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, log.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, log.namespace_name.c_str(), -1, SQLITE_TRANSIENT);
    if (log.user_id.empty()) {
        sqlite3_bind_null(stmt, 4);
    } else {
        sqlite3_bind_text(stmt, 4, log.user_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 5, log.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, log.content.c_str(), -1, SQLITE_TRANSIENT);
    if (log.query_type.empty()) {
        sqlite3_bind_null(stmt, 7);
    } else {
        sqlite3_bind_text(stmt, 7, log.query_type.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (log.status.empty()) {
        sqlite3_bind_null(stmt, 8);
    } else {
        sqlite3_bind_text(stmt, 8, log.status.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, 9, log.latency_ms);
    if (log.metadata_json.empty()) {
        sqlite3_bind_null(stmt, 10);
    } else {
        sqlite3_bind_text(stmt, 10, log.metadata_json.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 11, log.created_at.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Status::Internal("InteractionInsert failed: " + std::string(sqlite3_errmsg(db_)));
    }
    return Status::Ok();
}

Status MemoryStore::TouchSessionLocked(const std::string& session_id) {
    std::string now = NowISO8601();
    const char* sql =
        "UPDATE memory_sessions SET "
        "interaction_count = interaction_count + 1, "
        "updated_at = ? "
        "WHERE session_id = ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionTouch prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Status::Internal("SessionTouch failed: " + std::string(sqlite3_errmsg(db_)));
    }
    if (sqlite3_changes(db_) == 0) {
        return Status::NotFound("Session not found: " + session_id);
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Public locked methods (acquire mu_ then call locked helpers)
// ---------------------------------------------------------------------------

Status MemoryStore::InteractionInsert(InteractionLog& log) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);
    return InsertInteractionLocked(log);
}

Status MemoryStore::InteractionListBySession(const std::string& session_id,
                                             std::vector<InteractionLog>& interactions) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql =
        "SELECT id, session_id, namespace_name, user_id, role, content, "
        "query_type, status, latency_ms, metadata_json, created_at "
        "FROM interaction_log WHERE session_id = ? ORDER BY created_at ASC";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("InteractionListBySession prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    interactions.clear();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        interactions.push_back(ReadInteractionRow(stmt));
    }

    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::InteractionGetRecent(const std::string& session_id,
                                         int limit,
                                         std::vector<InteractionLog>& interactions) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql =
        "SELECT id, session_id, namespace_name, user_id, role, content, "
        "query_type, status, latency_ms, metadata_json, created_at "
        "FROM interaction_log WHERE session_id = ? ORDER BY created_at DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("InteractionGetRecent prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    interactions.clear();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        interactions.push_back(ReadInteractionRow(stmt));
    }

    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::InteractionSearch(const std::string& namespace_name,
                                      const std::string& query,
                                      const std::string& session_id,
                                      const std::string& user_id,
                                      int limit,
                                      std::vector<InteractionLog>& results) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    if (query.empty()) {
        return Status::InvalidArgument("search query is required");
    }
    if (limit < 1) limit = 10;
    if (limit > 100) limit = 100;

    // Build dynamic SQL with optional filters
    std::string sql =
        "SELECT id, session_id, namespace_name, user_id, role, content, "
        "query_type, status, latency_ms, metadata_json, created_at "
        "FROM interaction_log WHERE namespace_name = ? AND content LIKE ? ESCAPE '\\'";

    if (!session_id.empty()) {
        sql += " AND session_id = ?";
    }
    if (!user_id.empty()) {
        sql += " AND user_id = ?";
    }
    sql += " ORDER BY created_at DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("InteractionSearch prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    int bind_idx = 1;
    sqlite3_bind_text(stmt, bind_idx++, namespace_name.c_str(), -1, SQLITE_TRANSIENT);

    // LIKE pattern: escape wildcards in user query, then wrap in %..%
    std::string pattern = "%" + EscapeLikePattern(query) + "%";
    sqlite3_bind_text(stmt, bind_idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);

    if (!session_id.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, session_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!user_id.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, user_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bind_idx, limit);

    results.clear();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        results.push_back(ReadInteractionRow(stmt));
    }

    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::InteractionCount(const std::string& session_id, int64_t* count) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql = "SELECT COUNT(*) FROM interaction_log WHERE session_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("InteractionCount prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count = sqlite3_column_int64(stmt, 0);
    } else {
        *count = 0;
    }

    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::SessionCount(const std::string& namespace_name, int64_t* count,
                                 const std::string& user_id) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    // When a user_id is supplied, scope the count to that owner so the
    // total_count returned to a per-user list matches the filtered page.
    const std::string sql =
        std::string("SELECT COUNT(*) FROM memory_sessions WHERE namespace_name = ?") +
        (user_id.empty() ? "" : " AND user_id = ?");
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionCount prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, namespace_name.c_str(), -1, SQLITE_TRANSIENT);
    if (!user_id.empty()) {
        sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *count = sqlite3_column_int64(stmt, 0);
    } else {
        *count = 0;
    }

    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::SessionTouch(const std::string& session_id) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);
    return TouchSessionLocked(session_id);
}

Status MemoryStore::InteractionPairInsertAndSessionTouch(
        InteractionLog& user_log,
        InteractionLog& assistant_log,
        const std::string& session_id) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    // Begin transaction — mu_ is held for the entire duration, so no other
    // thread can interleave SQL statements on this db connection.
    char* err = nullptr;
    if (sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        return Status::Internal("Begin transaction failed: " + msg);
    }

    auto s = InsertInteractionLocked(user_log);
    if (!s.ok()) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return s;
    }

    s = InsertInteractionLocked(assistant_log);
    if (!s.ok()) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return s;
    }

    s = TouchSessionLocked(session_id);
    if (!s.ok()) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return s;
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return Status::Internal("Commit failed: " + msg);
    }

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Memory Immunity (opt-out) — memory_sessions.opt_out_at / opted_out_by
// ---------------------------------------------------------------------------

Status MemoryStore::SessionGetOptOut(const std::string& session_id,
                                     bool* exists,
                                     std::string* opt_out_at,
                                     std::string* opted_out_by) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    *exists = false;
    opt_out_at->clear();
    opted_out_by->clear();

    const char* sql =
        "SELECT opt_out_at, opted_out_by FROM memory_sessions WHERE session_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionGetOptOut prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *exists = true;
        *opt_out_at = SafeColText(stmt, 0);     // NULL → "" (active session)
        *opted_out_by = SafeColText(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return Status::Ok();
}

Status MemoryStore::SessionSetOptOut(const std::string& session_id,
                                     const std::string& opt_out_at,
                                     const std::string& opted_out_by) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql =
        "UPDATE memory_sessions SET opt_out_at = ?, opted_out_by = ? "
        "WHERE session_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionSetOptOut prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, opt_out_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, opted_out_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Status::Internal("SessionSetOptOut update: " + std::string(sqlite3_errmsg(db_)));
    }
    if (sqlite3_changes(db_) == 0) {
        return Status::NotFound("Session not found: " + session_id);
    }
    return Status::Ok();
}

Status MemoryStore::SessionClearOptOut(const std::string& session_id) {
    if (TryConsumeOpFault()) return Status::Internal("injected store failure");  // testing seam
    std::lock_guard<std::mutex> lock(mu_);

    const char* sql =
        "UPDATE memory_sessions SET opt_out_at = NULL, opted_out_by = NULL "
        "WHERE session_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return Status::Internal("SessionClearOptOut prepare: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return Status::Internal("SessionClearOptOut update: " + std::string(sqlite3_errmsg(db_)));
    }
    if (sqlite3_changes(db_) == 0) {
        return Status::NotFound("Session not found: " + session_id);
    }
    return Status::Ok();
}

}  // namespace cortrix
