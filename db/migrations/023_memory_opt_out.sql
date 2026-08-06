-- Memory Immunity — opt-out columns (issue/)
-- Wave C C-R2. Lives in the per-namespace memory DB (interaction_log / memory_sessions).
--
-- This file is the human-readable mirror of the DDL the runtime emits in
-- MemoryStore::MigrateMemoryOptOutColumns (src/memory/memory_store.cpp). The store is
-- the runtime SoT (applied on MemoryStore::Init); this file documents the same schema
-- for review / ops inspection. Keep the two in lock-step.
--
-- Dialect: SQLite (per-namespace memory DB). SQLite ADD COLUMN is NOT "if not exists",
-- so the runtime guards each ALTER on pragma_table_info; this mirror spells the plain
-- ALTER (apply once). All ADD — the frozen MVP columns are untouched (memory_sessions
-- session_id/namespace_name/user_id/title/interaction_count/doc_id/created_at/updated_at;
-- interaction_log id/session_id/namespace_name/user_id/role/content/query_type/status/
-- latency_ms/metadata_json/created_at).

-- memory_sessions: opt-out state (A+B double-field). NULL opt_out_at = active.
ALTER TABLE memory_sessions ADD COLUMN opt_out_at   TEXT DEFAULT NULL;  -- ISO 8601 opt-out time
ALTER TABLE memory_sessions ADD COLUMN opted_out_by TEXT DEFAULT NULL;  -- user_manual | agent_auto | system_auto | test

-- Partial index over opted-out sessions only (R6 — keeps is_session_opted_out cheap
-- without indexing the common active rows).
CREATE INDEX IF NOT EXISTS idx_memory_sessions_opt_out_at
    ON memory_sessions(opt_out_at) WHERE opt_out_at IS NOT NULL;

-- interaction_log: remember flag (remember=false on a new interaction triggers
-- session-level opt-out). DEFAULT 1 (TRUE): existing/new interactions are remembered.
ALTER TABLE interaction_log ADD COLUMN remember BOOLEAN DEFAULT 1;
