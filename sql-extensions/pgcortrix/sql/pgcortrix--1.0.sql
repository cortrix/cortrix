-- pgcortrix--1.0.sql — Cortrix PostgreSQL extension, V1 (plpython3u + HTTP REST).
--
-- SoT: design/features/F14-pgcortrix.md v1.0.3 (§2.1 signatures, §2.2 GUC, §3.3
-- endpoint map). Loaded by `CREATE EXTENSION pgcortrix`.
--
-- Shape of V1 (F14 §1, L0' 2026-05-07): every function is a thin plpython3u
-- wrapper that calls the Cortrix Server HTTP API via the pgcortrix_helper Python
-- module (installed into PG's plpython3u site-packages). No shared memory / C
-- code — that is the V3+ roadmap (§9). The Python logic is kept in the helper
-- module (not inlined here) so it is unit-testable off a live PG (mock urllib),
-- which is how the standalone DoD is met (L1-α briefing §6).
--
-- Function inventory (F14 §2.1): 5 main + 2 helper.
--   main:   pgcortrix_search / pgcortrix_upload / pgcortrix_list_documents /
--           pgcortrix_memory_search / pgcortrix_list_interactions
--   helper: pgcortrix_configure / pgcortrix_status

\echo Use "CREATE EXTENSION pgcortrix" to load this file. \quit

-- ===========================================================================
-- §2.1.1  Composite return types (4)
-- ===========================================================================
-- Four SETOF-returning functions need a composite row type each. Column sets
-- mirror the HTTP response schemas these functions wrap (F14 §3.3 → ARCH §4.1).

CREATE TYPE pgcortrix_search_result AS (
    chunk_id     TEXT,
    content      TEXT,
    score        FLOAT,
    rerank_score FLOAT,
    doc_id       TEXT,
    filename     TEXT,
    metadata     JSONB
);

CREATE TYPE pgcortrix_doc_info AS (
    doc_id     TEXT,
    filename   TEXT,
    status     TEXT,
    chunks     INT,
    created_at TIMESTAMPTZ
);

CREATE TYPE pgcortrix_memory_result AS (
    memory_id   TEXT,
    content     TEXT,
    score       FLOAT,       -- raw RRF score (pre-decay)
    created_at  TIMESTAMPTZ,
    memory_type TEXT,        -- fact | preference | event
    status      TEXT,        -- active | invalidated
    final_score FLOAT        -- MEM01: raw * decay_factor (ranking key, design § 2.1)
);

-- v1.0.1: interaction_log list type. MEM05 v1.0 D4 forces per-user isolation,
-- so pgcortrix_list_interactions always carries user_id (§2.1.2 fn 5).
CREATE TYPE pgcortrix_interaction_info AS (
    interaction_id TEXT,
    user_id        TEXT,
    session_id     TEXT,
    role           TEXT,        -- user | assistant | system
    content        TEXT,
    created_at     TIMESTAMPTZ,
    metadata       JSONB
);

-- ===========================================================================
-- §2.2  GUC configuration (4)
-- ===========================================================================
-- plpython3u has no PGC_SUSET hook of its own, so we register the GUCs as
-- placeholder custom GUCs via set_config(). `pgcortrix.endpoint` MUST be SUSET
-- (superuser-only) per V3-E-02 (2026-05-23): a USERSET endpoint is an SSRF
-- vector (SET pgcortrix.endpoint='http://169.254.169.254/...' → cloud metadata).
-- The helper additionally validates the endpoint against an allowlist + private
-- IP blocklist on every call (§2.2.bis), so this is defence-in-depth.
--
-- NOTE: a packaged build registers these as real custom GUCs in a small C shim
-- (or via `ALTER SYSTEM` + custom_variable_classes). V1 uses set_config so the
-- pure-SQL/plpython3u extension installs with no compiled module.
DO $$
BEGIN
    PERFORM set_config('pgcortrix.endpoint',   'http://localhost:9090', false);
    PERFORM set_config('pgcortrix.api_key',    '',                      false);
    PERFORM set_config('pgcortrix.timeout_ms', '30000',                 false);
    PERFORM set_config('pgcortrix.retry_max',  '3',                     false);
END $$;

-- ===========================================================================
-- §2.1.2  Main functions (5)
-- ===========================================================================
-- Each body defers to the cached PgcortrixClient helper (GD-cached so the module
-- is imported once per backend, not per call — F14 §2.3). The helper does the
-- GUC read, endpoint validation, HTTP call, retry, cancel handling and error
-- mapping; the SQL layer only marshals args in and yields rows / values out.

-- 1. Semantic search.
CREATE FUNCTION pgcortrix_search(
    namespace TEXT,
    query     TEXT,
    top_k     INT     DEFAULT 10,
    filter    JSONB   DEFAULT NULL,
    rerank    BOOLEAN DEFAULT TRUE
) RETURNS SETOF pgcortrix_search_result
LANGUAGE plpython3u
STABLE                          -- L3.4: LATERAL JOIN friendly
PARALLEL SAFE
AS $$
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    for r in client.search(namespace, query, top_k, filter, rerank):
        yield (
            r.get('chunk_id'),
            r.get('content'),
            r.get('score'),
            r.get('rerank_score'),
            r.get('doc_id'),
            r.get('filename'),
            r.get('metadata'),
        )
$$;

-- 2. Document upload.
CREATE FUNCTION pgcortrix_upload(
    namespace TEXT,
    file_path TEXT
) RETURNS TEXT                   -- doc_id
LANGUAGE plpython3u
VOLATILE                         -- write path
AS $$
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    return client.upload(namespace, file_path)
$$;

-- 3. List documents in a namespace.
CREATE FUNCTION pgcortrix_list_documents(
    namespace TEXT
) RETURNS SETOF pgcortrix_doc_info
LANGUAGE plpython3u
STABLE
PARALLEL SAFE
AS $$
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    for r in client.list_documents(namespace):
        yield (
            r.get('doc_id'),
            r.get('filename'),
            r.get('status'),
            r.get('chunks'),
            r.get('created_at'),
        )
$$;

-- 4. Memory search (MEM05: user_id is mandatory for per-user isolation).
CREATE FUNCTION pgcortrix_memory_search(
    namespace TEXT,
    query     TEXT,
    user_id   TEXT,              -- MEM05 forced per-user isolation
    top_k     INT  DEFAULT 5
) RETURNS SETOF pgcortrix_memory_result
LANGUAGE plpython3u
STABLE
PARALLEL SAFE
AS $$
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    for r in client.memory_search(namespace, query, user_id, top_k):
        yield (
            r.get('memory_id'),
            r.get('content'),
            r.get('score'),
            r.get('created_at'),
            r.get('memory_type'),
            r.get('status'),
            r.get('final_score'),    # MEM01: decayed ranking key
        )
$$;

-- 5. interaction_log list (v1.0.2 — D1 V3 decision 4 "combined" shape).
-- MEM05 v1.0 D4 three-way parity = user_id mandatory (security) + filter capability aligned
-- with F13 §8.3 HTTP API (function). Three-way parity with MCP
-- cortrix_list_interactions + HTTP GET /memory/interactions.
--   filter JSONB whitelist (§2.1.5): session_id / namespace_id / from_ts / to_ts
--   / sort_order. Anything else → CX_ERR_F14_INVALID_FILTER.
CREATE FUNCTION pgcortrix_list_interactions(
    namespace TEXT,
    user_id   TEXT,                  -- MEM05 D4: mandatory positional (no NULL/default)
    filter    JSONB DEFAULT NULL,    -- v1.0.2 optional whitelist filter
    limit_n   INT   DEFAULT 50,
    offset_n  INT   DEFAULT 0
) RETURNS SETOF pgcortrix_interaction_info
LANGUAGE plpython3u
STABLE
PARALLEL SAFE
AS $$
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    for r in client.list_interactions(namespace, user_id, filter, limit_n, offset_n):
        yield (
            r.get('interaction_id'),
            r.get('user_id'),
            r.get('session_id'),
            r.get('role'),
            r.get('content'),
            r.get('created_at'),
            r.get('metadata'),
        )
$$;

-- ===========================================================================
-- §2.1.3  Helper functions (2)  — main = 1..5, helpers numbered from 6
-- ===========================================================================

-- 6. Session-level API key configuration.
CREATE FUNCTION pgcortrix_configure(api_key TEXT)
RETURNS VOID
LANGUAGE plpython3u
VOLATILE
AS $$
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    client.configure(api_key)
$$;

-- 7. Status + connection diagnostics (V23 D6: unified status() JSONB, pg_stat_*
-- style). Returns a JSONB blob: version / http_connected / endpoint / latency_ms
-- / timeout_ms / retry_max (forward-compatible — V3+ adds slot_used etc.).
CREATE FUNCTION pgcortrix_status()
RETURNS JSONB
LANGUAGE plpython3u
STABLE
AS $$
    import json
    from pgcortrix_helper import get_client
    client = get_client(plpy, GD)
    return json.dumps(client.status())
$$;
