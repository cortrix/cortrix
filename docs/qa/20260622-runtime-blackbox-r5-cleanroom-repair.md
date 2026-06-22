# Runtime Black-box Round-5 Clean-room Repair PR Guide

Status: `ready for R&D review`

Related Hub feedback package:

`dev-cortrix-hub/market/rd-feedback/20260622-cortrix-runtime-blackbox-r5-feedback/`

## Purpose

This PR applies lightweight, source-first repairs found during the fifth-round clean-room runtime black-box run. The run started from Cortrix `origin/main` at:

```text
6183d07 feat(llm): log low-level transport failures (was swallowed as generic timeout)
```

The patch intentionally stays on API/UI/static HTTP surface compatibility. It does not change core storage architecture, tenant authorization design, MEM02 extraction internals, or the product model for auth-disabled local runtime.

## First Reading Order

1. This file.
2. `src/server/http_server.cpp`
3. `src/server/routes/agent_proxy_routes.cpp`
4. `web/src/api/batch.ts`
5. `web/src/api/memory.ts`
6. `web/src/components/Memory/MemoryPage.tsx`
7. `web/src/pages/admin/UsersPage.tsx`
8. `web/src/types/api.ts`
9. `web/src/api/mock.ts`
10. Fifth-round evidence matrix in `CortrixGTM/project-ops/cortrix-runtime-validation/20260622-agent-black-box-round5/repair/round-01/test-results-matrix.md`

## Changed Areas

### Agent proxy invalid request handling

- Same-origin `/api/v1/agent/chat` now preserves upstream non-SSE 4xx JSON instead of returning HTTP 200 with a failed SSE stream.
- Valid chat streaming still returns the upstream SSE stream.

Review notes:

- This is a protocol correctness fix for invalid request behavior.
- The PR does not change the Agent service contract or implement new Agent routes.

### OpenAPI and static HTTP surface

- `/openapi.yaml` is now explicitly registered.
- JSON API responses and JSON errors now receive baseline security headers: `X-Frame-Options`, `X-Content-Type-Options`, and `Referrer-Policy`.
- SPA fallback no longer serves `index.html` for missing `/assets/*` or file-like paths; missing static assets now return a structured 404 JSON response.

Review notes:

- The static fallback change prevents missing asset requests from looking like successful UI page loads.
- Existing Web UI security headers are still applied to real SPA pages.

### Web UI batch endpoint

- Bulk upload now calls the current backend endpoint `/api/v1/documents/batch` instead of stale `/api/v1/documents/batch-submit`.

Review notes:

- This matches the backend route observed during the clean-room run.

### Web UI Memory edit / invalidate contract

- Memory edit now sends backend-required `namespace`, `user_id`, `content`, and `memory_type` fields while keeping compatibility fields.
- Memory invalidate now sends `namespace` and `user_id` query parameters.
- UI/mock response types now match backend invalidate acknowledgement shape: `{block_id, status}`.

Review notes:

- This keeps the UI aligned with the current API shape without inventing a new memory model.

### Admin Users created_at rendering

- Admin Users UI now accepts ISO strings, Unix seconds, and Unix milliseconds for `created_at`.

Review notes:

- Clean-room API evidence returned numeric timestamps.
- This is a render-layer compatibility fix only.

## Fifth-round Evidence

Canonical evidence root:

```text
CortrixGTM/project-ops/cortrix-runtime-validation/20260622-agent-black-box-round5/
```

Primary reading order:

1. `README.md`
2. `repair/round-01/test-results-matrix.md`
3. `repair/round-01/run-log.md`
4. `repair/round-01/ui/report.md`
5. `repair/round-01/ui/screenshot-quality-check.md`
6. `repair/round-01/api/report.md`
7. `repair/round-01/api/tenant-acl-quota-report.md`
8. `repair/round-01/mcp-sdk/summary.md`
9. `repair/round-01/security/security-runtime-report.md`
10. `repair/round-01/runtime/runtime-summary.md`

Latest direct result:

| Area | Result |
|---|---:|
| Final UI screenshots | 19 screenshots, all distinct and non-empty |
| UI final replay | 18 pass + 1 expected-negative pass / 0 fail |
| Python MCP full suite | 79 passed |
| Python SDK full suite | 188 passed |
| Targeted C++ HTTP surface tests | 16/16 passed |
| Real MCP-to-API probe | 32 events / 1 hard failure |

## Verification Already Run

Recorded in the evidence root:

```text
cmake --build build --target cortrix-server
npm run build
ctest -R "HealthEndpointTest|NamespaceApiTest|FullHttpE2ETest.ErrorFormatConsistency" --output-on-failure
node project-ops/cortrix-runtime-validation/20260622-agent-black-box-round5/repair/round-01/ui/ui-round5-probe.mjs
node project-ops/cortrix-runtime-validation/20260622-agent-black-box-round5/repair/round-01/api/tenant-acl-quota-probe.mjs
node project-ops/cortrix-runtime-validation/20260622-agent-black-box-round5/repair/round-01/security/security-runtime-probe.mjs
python -m pytest cortrix-mcp/tests
python -m pytest sdk/python/tests
```

Results:

- Backend build passed.
- Frontend production build passed.
- Targeted C++ HTTP surface tests passed: 16/16.
- Final UI clean-room replay passed: 18 pass + 1 expected-negative pass.
- Final screenshot quality check passed: 19 unique screenshot hashes, no near-empty screenshot.
- Full Python MCP suite passed: 79 tests.
- Full Python SDK suite passed: 188 tests.

## Remaining Findings Not Solved In This PR

| ID | Surface | Status | Why it remains open |
|---|---|---|---|
| `R5-API-013` | API MEM02 extraction | `fail` | API returns `CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT`; batch extraction reports failed item. |
| `R5-MCP-006` | MCP memory extract | `fail` | MCP real HTTP probe reaches the same backend MEM02 timeout. |
| `RB5-MEM02-JSON-FENCE-005` | MEM02 fenced/prose parser path | `blocked_mem02_transport` | Parser behavior cannot be proven because the configured LLM transport path fails first. |
| `RB5-MEM02-TIMEOUT-CFG-006` | MEM02 timeout config | `blocked_mem02_transport` | Runtime reaches the same MEM02/GLM transport failure. |
| `R5-API-007` | Auth login contract | `fail_contract_drift` | OpenAPI/docs expose login, but `POST /api/v1/auth/login` returns 404 in this runtime. |
| `R5-API-021` | Tenant/member/ACL/quota | `fail_contract_drift` | Routes are callable, but request/response schemas drift from OpenAPI; member add returned 201 while subsequent list remained empty. |
| `R5-SEC-001` | RBAC deny matrix | `blocked_auth_disabled` | Clean-room `/auth/me` reports `auth_disabled=true`, so real deny behavior cannot be proven. |
| `R5-SEC-002` | Tenant isolation deny matrix | `blocked_auth_disabled` | Same auth-disabled boundary. |
| `R5-RUNTIME-003` | Runtime startup security event | `security_event` | LogSanitizer config is missing and sanitizer is disabled in captured startup. |

## R&D Decisions Requested

1. Accept, replace, or split the HTTP/static/API surface fixes in this PR.
2. Decide whether `/openapi.yaml` should remain a root canonical asset or require a packaging/build artifact gate.
3. Decide whether baseline JSON response security headers should be centralized differently.
4. Decide whether Web UI should keep compatibility payloads for Memory edit or move to a single canonical API shape.
5. Decide whether Admin Users API should guarantee ISO timestamps or whether UI should continue accepting numeric timestamps.
6. Decide whether MEM02 timeout should block merge or remain a known backend issue.
7. Decide whether auth login should be documented as unavailable in this runtime or implemented.
8. Decide whether tenant/member/ACL/quota should be fixed in implementation, OpenAPI, or both.
9. Decide how to test RBAC and tenant isolation outside auth-disabled local mode.
10. Decide whether LogSanitizer config absence is acceptable in local dev or must be packaged by default.

## Files Intentionally Not Included

The local working tree also contains an untracked `benchmarks/` directory with files dated 2026-06-14. It has no Git history under this repo and is not part of the Round-5 repair patch.

## Hub Boundary

The paired Hub package remains `needs_rd_review`. This source PR should not be interpreted as a public release-readiness claim, benchmark result, or Hub design SoT promotion.

Raw evidence is preserved 1:1 in the private CortrixGTM evidence root. This source PR summarizes and links that evidence; it does not sanitize or replace it.
