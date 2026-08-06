#include "cortrix/agent_trace/agent_trace_schema.h"

#include <sqlite3.h>

#include <string>

namespace cortrix::agent_trace {

// agent_trace schema — transcribed 1:1 from the design. Kept as one DDL
// batch so the migrator applies it atomically. IF NOT EXISTS on every object so a
// re-run on an already-migrated db is a no-op even outside the migrator's version
// gate (defensive; the migrator's schema_version gate is the primary idempotency
// mechanism). VARCHAR(n) type names are kept verbatim (SQLite TEXT affinity); see
// agent_trace_schema.h for the dialect note. created_at is Unix ms (topic 3 —
// same clock as operation_log for the trace_id correlation chain).
const char* const kAgentTraceSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS agent_trace (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,

    -- Session identity (topic 3 — VARCHAR(64) → VARCHAR(128))
    session_id    VARCHAR(128),                -- Agent session id (C2; same length as operation_log.session_id)
    trace_id      VARCHAR(128),                -- call-chain id (C1; same length as operation_log.trace_id)
    agent_id      VARCHAR(128),                -- Agent identity (topic 3 — length aligned)

    -- call detail
    method        VARCHAR(64) NOT NULL,        -- search / upload / memory_search / session_end / session_timeout
    params        TEXT,                        -- call params JSON (topic 3 — ≤2KB, keep head+tail on truncate)
    result_summary TEXT,                       -- result summary (topic 3 — ≤512, keep head+tail; failure keeps error only)
    duration_ms   INTEGER,
    source        VARCHAR(16),                 -- "http" | "mcp"

    -- v1.0.6 added (topic 3)
    status        VARCHAR(16) DEFAULT 'success', -- success / failed / cancelled / session_timeout
    error_code    VARCHAR(64),                 -- failure error code (e.g. "CX_ERR_NS_NOT_FOUND")
    namespace_id  VARCHAR(64),                 -- cross-NS filter dimension (NULL allowed — global call)

    created_at    INTEGER NOT NULL             -- Unix ms (topic 3 — same clock as operation_log)
);

-- indices (session + agent + trace-id correlation chain)
CREATE INDEX IF NOT EXISTS idx_agent_trace_session  ON agent_trace(session_id, created_at);
CREATE INDEX IF NOT EXISTS idx_agent_trace_agent    ON agent_trace(agent_id, created_at);
CREATE INDEX IF NOT EXISTS idx_agent_trace_trace_id ON agent_trace(trace_id, created_at);
)SQL";

Status AgentTraceSchemaProvider::Migrate(sqlite3* db, int from_ver, int to_ver) {
    // Phase 1: 0 → 1 single-step creation. Accept an already-current (1 → 1) call
    // defensively. Phase 2 internal evolution will branch on (from_ver, to_ver).
    if (from_ver == to_ver) {
        return Status::Ok();  // already current
    }
    if (from_ver == 0 && to_ver == kAgentTraceSchemaVersion) {
        char* err = nullptr;
        if (sqlite3_exec(db, kAgentTraceSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            return Status::Internal(
                std::string("CX_ERR_TRACE_INTERNAL: agent_trace agent_trace schema migration failed: ") + msg);
        }
        return Status::Ok();
    }
    // Unexpected version step (e.g. Phase 2 1 → 2) is not implemented yet.
    return Status::InvalidArgument(
        "CX_ERR_TRACE_INTERNAL: agent_trace unsupported migration " +
        std::to_string(from_ver) + " -> " + std::to_string(to_ver));
}

}  // namespace cortrix::agent_trace
