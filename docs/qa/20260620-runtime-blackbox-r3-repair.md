# Runtime Black-box Round-3 Repair PR Guide

Status: `ready for R&D review`

Related Hub feedback package:

`dev-cortrix-hub/market/rd-feedback/20260620-cortrix-runtime-blackbox-r3-repair-feedback/`

## Purpose

This PR applies lightweight API-first fixes from the Round-3 runtime black-box repair loop. It does not add new public product surfaces beyond the existing route/API contracts under review, and it does not change architecture boundaries.

The goal is to make the current runtime pass the agreed local black-box gate while keeping the remaining failures visible to R&D.

## First Reading Order

1. This file.
2. `src/server/http_server.cpp` and `include/cortrix/server/http_server.h`
3. `src/auth/auth_middleware.cpp`
4. `sdk/python/cortrix/resources/memory.py`
5. `sdk/python/cortrix/types/lists.py`
6. `cortrix-mcp/src/cortrix_mcp/tools/admin.py`
7. `cortrix-mcp/src/cortrix_mcp/tools/core.py`
8. `cortrix-mcp/src/cortrix_mcp/tools/memory.py`
9. SDK test updates under `sdk/python/tests/`
10. `dev.sh`

## Changed Areas

### HTTP / API Runtime

- Adds `/docs` as a local Swagger UI entry.
- Adds `/openapi.bundled.yaml` with fallback to `api/openapi.yaml`.
- Adds `/openapi.json` with fallback JSON conversion from YAML when no JSON artifact exists.
- Adds namespace stats route support for UI/runtime validation.
- Adds `RegisterOpenApiRoutes()` to route registration.

Review notes:

- The OpenAPI route fallback is intentionally pragmatic for local runtime validation.
- R&D should decide whether bundled OpenAPI generation must become a packaging gate.
- Namespace stats includes compatibility fields needed by the current UI/API surface; some deeper storage/query-time metrics remain placeholders.

### Local Dev Runtime

- `dev.sh` exports `CORTRIX_OPENAPI_ROOT`.
- `dev.sh` passes `CORTRIX_AGENT_BASE_URL` to backend when a local Agent runtime is available.
- Agent port/base URL now respect `CORTRIX_AGENT_PORT` and `CORTRIX_AGENT_BASE_URL`.

Review notes:

- This is local development wiring, not a deployment architecture change.
- R&D should confirm no hard-coded local port assumption remains where production deployment expects a different agent route.

### Auth-disabled Context

- `src/auth/auth_middleware.cpp` now provides compatible tenant/user context when auth is disabled.

Review notes:

- This supports local validation and admin/tool flows in CE/no-auth mode.
- R&D should review whether the default context values match the intended CE no-auth semantics.

### Python SDK

- `system.health()` now targets `/system/health/live`.
- Memory search maps to the live `results` response shape.
- Memory create/edit/delete return acknowledgement models rather than full `Memory` objects.
- Memory update/delete/opt-out can carry namespace context; the client remembers namespace for resources created or logged through that client.
- Ops resource listing is aligned with runtime output.

Review notes:

- Namespace backfill is a client-side compatibility cache. If a memory/session was created outside the current client, callers should pass `namespace` explicitly.
- The return-model change is a contract-level change and should be reviewed against SDK versioning expectations.

### MCP

- `cortrix_memory_log` creates a missing session and retries when the live API returns the session-not-found precondition.
- `cortrix_memory_revoke` now uses `/memory/invalidations/{memory_id}/revoke`.
- `cortrix_memory_edit` includes namespace.
- Admin credential registration accepts compatibility parameters such as `connection_ref` and `description`.
- Admin import maps an alias `connection_ref` to the backend returned ref ID within the current MCP process.

Review notes:

- `_CONNECTION_REF_ALIASES` is process-local compatibility state and is not durable.
- The memory-log retry currently keys off known session-not-found error text/code. R&D can replace it with a more structured transport error if desired.
- The remaining import-filter failure is not hidden; see residual failures below.

## Final Local Validation Result

Official evidence root:

`CortrixGTM/project-ops/cortrix-runtime-validation/20260619-agent-black-box-round3-repair/`

Official round:

`round-03-0539168-api-first-repair`

| Area | Result |
|---|---:|
| Cortrix HEAD under test | `05391682db670452ebf8a92a616a7d76f2215377` |
| API/SDK/MCP total | 97 |
| API/SDK/MCP pass | 93 |
| API/SDK/MCP fail | 4 |
| UI total steps | 16 |
| UI errors | 0 |
| Final pass rate | `95.88%` |

Checks recorded in the evidence package:

- `bash -n dev.sh`
- SDK/MCP `py_compile`
- `git diff --check`
- `cmake --build ... --target cortrix-server -j2`
- Targeted smoke script for repaired paths
- Full clean-run script for the final black-box matrix

## Residual Failures

These failures are intentionally preserved for R&D review:

| ID | Surface | Error |
|---|---|---|
| `sdk-memory-extract` | SDK | `CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT` |
| `mcp-memory-extract-trigger` | MCP | `CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT` |
| `mcp-memory-extract` | MCP | `CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT` |
| `mcp-admin-import-route` | MCP/Admin Import | `CX_ERR_F16A_INVALID_SQL: filter must be a JSON object` |

## R&D Decisions Requested

1. Accept, replace, or split the OpenAPI/docs route fallback.
2. Accept, replace, or harden the CE no-auth default context.
3. Decide whether SDK memory ACK return models require a versioning note.
4. Decide whether MCP admin aliasing should stay process-local or move to a structured API flow.
5. Decide whether MEM02 timeout should block merge or remain a known residual issue.
6. Decide whether F16a import filters should accept strings, MCP should coerce them, or docs should require JSON object filters only.
7. Decide whether namespace stats placeholder fields need immediate implementation before merge.

## Files Intentionally Not Included

The local working tree also contains untracked `benchmarks/` files. They are not part of this PR and should not be reviewed as part of the Round-3 repair patch.

## Hub Boundary

The Hub feedback package remains `needs_rd_review`. This source PR should not be interpreted as a public release-readiness claim or benchmark result.
