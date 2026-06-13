# pgcortrix — Cortrix as a PostgreSQL extension (V1)

Call Cortrix semantic retrieval, document upload and memory query directly from
SQL — no HTTP client needed in your app. Under the hood `pgcortrix` is a
`plpython3u` wrapper over the Cortrix Server HTTP REST API.

> **SoT**: [`design/features/F14-pgcortrix.md`](../../../dev-cortrix-hub/design/features/F14-pgcortrix.md)
> (v1.0.3). This directory is the F14 V1 implementation. Built **independently**
> with PGXS — it is *not* part of the main `cortrix/` CMake build.

## Why plpython3u (and not a C extension)

F14 V1 (L0' route, 2026-05-07) deliberately chose `plpython3u` + HTTP over a C
extension + shared-memory IPC:

- **PG-version-agnostic** — works on PG 13–17 via the stable `plpython3u` ABI.
- **Fast to evolve** — a function-body change ships without recompiling PG.
- **Fault isolation** — Cortrix Server is a separate process; a crash there does
  not take down PG.
- **Cloud-friendly** — Cloud V1.5 swaps the transport for `aws_lambda` on RDS PG
  with identical function signatures.

The shared-memory IPC design (`response_buf=1MB`, 32-slot state machine, etc.) is
the **V3+ roadmap** (§9 of the SoT), not V1.

## Layout

| Path | Contents |
|---|---|
| `pgcortrix.control` | Extension control file (`superuser`, `requires plpython3u`). |
| `sql/pgcortrix--1.0.sql` | 4 composite types + 4 GUCs + 7 functions (5 main + 2 helper). |
| `python/pgcortrix_helper.py` | HTTP client (`urllib`), GUC read, retry, cancel, SSRF guard, `user_id` synthesis, error model. |
| `Makefile` | PGXS build + Python helper install. |
| `tests/` | Python unit tests (mock `urllib`, fake `plpy`) — run without a live PG. |

## Functions (F14 §2.1)

**Main (5):**

| Function | Returns | HTTP endpoint (§3.3) |
|---|---|---|
| `pgcortrix_search(namespace, query, top_k, filter, rerank)` | `SETOF pgcortrix_search_result` | `POST /api/v1/query` |
| `pgcortrix_upload(namespace, file_path)` | `TEXT` (doc_id) | `POST /api/v1/documents` |
| `pgcortrix_list_documents(namespace)` | `SETOF pgcortrix_doc_info` | `GET /api/v1/namespaces/{ns}/documents` |
| `pgcortrix_memory_search(namespace, query, user_id, top_k)` | `SETOF pgcortrix_memory_result` | `POST /api/v1/memory/search` |
| `pgcortrix_list_interactions(namespace, user_id, filter, limit_n, offset_n)` | `SETOF pgcortrix_interaction_info` | `GET /api/v1/memory/interactions` |

**Helper (2):** `pgcortrix_configure(api_key) → VOID`, `pgcortrix_status() → JSONB`.

`pgcortrix_memory_search` / `pgcortrix_list_interactions` require `user_id`
(MEM05 per-user isolation — three-way parity with the MCP + HTTP layers).

## Configuration (GUC, §2.2)

| GUC | Default | Scope | Notes |
|---|---|---|---|
| `pgcortrix.endpoint` | `http://localhost:9090` | **SUSET** | Superuser-only (SSRF defence, V3-E-02). |
| `pgcortrix.api_key` | `''` | USERSET | Empty = anonymous (CE default). |
| `pgcortrix.timeout_ms` | `30000` | USERSET | HTTP request timeout. |
| `pgcortrix.retry_max` | `3` | USERSET | Max retries on 5xx. |

## Install (requires a live PG — D3.5)

```bash
make && make install          # needs pg_config on PATH + plpython3u available
psql -c "CREATE EXTENSION pgcortrix CASCADE;"   # CASCADE pulls in plpython3u
```

## Test (standalone — no PG needed)

```bash
python3 -m unittest discover -s tests -v
```

- `tests/test_sql_contract.py` — self-checks the SQL DDL contract (types,
  columns, signatures, params/defaults, volatility, GUCs).
- `tests/test_helper.py` — the HTTP client with `urllib` + `plpy` faked: call
  shape per endpoint, `user_id` synthesis/enforcement, filter whitelist, SSRF,
  retry/backoff, cancel, `status()`.
- `tests/test_sql_helper_seam.py` — asserts every `client.<m>(...)` call in the
  SQL matches a helper method of the same arity (catches SQL/Python drift).

Live PG load + `CREATE EXTENSION` + `pg_regress` integration is **D3.5** (the
build machine has no PostgreSQL).
