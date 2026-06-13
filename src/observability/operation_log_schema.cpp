#include "cortrix/observability/operation_log_schema.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::observability {

// operation_log schema — transcribed 1:1 from F18a §5.1. Kept as one DDL batch so
// the migrator applies it atomically. IF NOT EXISTS on every object so a re-run on
// an already-migrated db is a no-op even outside the migrator's version gate
// (defensive; the migrator's schema_version gate is the primary idempotency
// mechanism). VARCHAR(128) type names are kept verbatim (SQLite TEXT affinity);
// see operation_log_schema.h for the dialect note.
const char* const kOperationLogSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS operation_log (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp     INTEGER NOT NULL,            -- Unix ms

    -- user identity: real user_id / "anonymous" / "pg:<PG_user>:<pid>" (topic 8)
    user_id       TEXT NOT NULL,

    -- operation semantics: {resource}_{verb}, len<=32, lowercase, '_' (topic 1)
    action        TEXT NOT NULL,

    -- resource association
    namespace_id  TEXT,                        -- optional
    resource_type TEXT NOT NULL,               -- document/namespace/memory/query/db_connection/db_import
    resource_id   TEXT,                        -- interaction_id / doc_id / block_id / namespace_id / ...

    -- summary (<=100 chars; "[auto] " prefix for LLM-extracted)
    summary       TEXT,

    -- correlation chain (v1.0.6) — NULL allowed (cron / internal cleanup has no trace)
    trace_id      VARCHAR(128),                -- topic 6 — F13 agent_trace.trace_id (same call)
    session_id    VARCHAR(128)                 -- C2     — F13 agent_trace.session_id (same session)
);

-- indices (§5.1: 2 baseline + 3 added by topic 2/6 + C2)
CREATE INDEX IF NOT EXISTS idx_oplog_timestamp     ON operation_log(timestamp);
CREATE INDEX IF NOT EXISTS idx_oplog_user_action   ON operation_log(user_id, action, timestamp);
CREATE INDEX IF NOT EXISTS idx_oplog_trace_id      ON operation_log(trace_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_oplog_session_id    ON operation_log(session_id, timestamp);
CREATE INDEX IF NOT EXISTS idx_oplog_user_resource ON operation_log(user_id, resource_type, timestamp);
)SQL";

Status OperationLogSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 single-step creation. Accept an already-current (1 → 1) call
    // defensively. Phase 2 internal evolution will branch on (from_ver, to_ver).
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == kOplogSchemaVersion) {
        char* err = nullptr;
        if (sqlite3_exec(db, kOperationLogSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            return Status::Internal(
                std::string("CX_ERR_OPLOG_INTERNAL: F18a operation_log schema migration failed: ") + msg);
        }
        return Status::Ok();
    }
    // Unexpected version step (e.g. Phase 2 1 → 2) is not implemented yet.
    return Status::InvalidArgument(
        "CX_ERR_SCHEMA_VERSION_MISMATCH: F18a unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::observability
