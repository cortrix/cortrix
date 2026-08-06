-- Agent Observability — agent_trace (topic 3)
-- Wave C C-R1. Lives in the global DB (cortrix_global.db), NOT a per-namespace DB.
--
-- This file is the human-readable mirror of the DDL the AgentTraceSchemaProvider
-- emits (src/agent_trace/agent_trace_schema.cpp kAgentTraceSchemaSql). The provider
-- is the runtime SoT (applied atomically via the catalog SchemaMigrator against the
-- global DB, alongside the OperationLogSchemaProvider); this file documents the
-- same schema for review / ops inspection. Keep the two in lock-step.
--
-- Dialect: SQLite (global DB). The agent trace spec spells session_id/trace_id/agent_id
-- as VARCHAR(128); SQLite accepts VARCHAR(n) as a type name (TEXT affinity, no length
-- enforcement), kept verbatim for spec fidelity. created_at is Unix-ms INTEGER (same
-- clock as operation_log so the trace_id correlation chain joins cleanly). IF NOT
-- EXISTS on every object.
--
-- Open-Core: this is the CE-only schema. Ent's agent_trace_extension table
-- (input_tokens / output_tokens / query_pattern_id) is a SEPARATE migration in
-- a separate distribution keyed by agent_trace.id — it does NOT alter this
-- table (agent trace extension-table isolation).

CREATE TABLE IF NOT EXISTS agent_trace (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,

    -- Session identity (topic 3 — VARCHAR(64) → VARCHAR(128))
    session_id    VARCHAR(128),                  -- Agent session id (C2; == operation_log.session_id length)
    trace_id      VARCHAR(128),                  -- call-chain id (== operation_log.trace_id length)
    agent_id      VARCHAR(128),                  -- Agent identity (topic 3 — length aligned)

    -- call detail
    method        VARCHAR(64) NOT NULL,          -- search / upload / memory_search / session_end / session_timeout
    params        TEXT,                          -- call params JSON (topic 3 — ≤2KB, keep head+tail on truncate)
    result_summary TEXT,                         -- result summary (topic 3 — ≤512; failure keeps error only)
    duration_ms   INTEGER,
    source        VARCHAR(16),                   -- "http" | "mcp"

    -- v1.0.6 added (topic 3)
    status        VARCHAR(16) DEFAULT 'success', -- success / failed / cancelled / session_timeout
    error_code    VARCHAR(64),                   -- failure error code (e.g. "CX_ERR_NS_NOT_FOUND")
    namespace_id  VARCHAR(64),                   -- cross-NS filter dimension (NULL allowed — global call)

    created_at    INTEGER NOT NULL               -- Unix ms (topic 3 — same clock as operation_log)
);

CREATE INDEX IF NOT EXISTS idx_agent_trace_session  ON agent_trace(session_id, created_at);
CREATE INDEX IF NOT EXISTS idx_agent_trace_agent    ON agent_trace(agent_id, created_at);
CREATE INDEX IF NOT EXISTS idx_agent_trace_trace_id ON agent_trace(trace_id, created_at);
