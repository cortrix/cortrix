# Runtime Black-box Round-4 RD Refresh Repair PR Guide

Status: `ready for R&D review`

Related Hub feedback package:

`dev-cortrix-hub/market/rd-feedback/20260621-cortrix-runtime-blackbox-r4-rd-refresh-feedback/`

## Purpose

This PR applies lightweight, API/source-first repairs from the fourth-round clean-room runtime black-box run after the R&D refresh. Before PR submission the branch was rebased onto `origin/main` at `8eb5b6a` (`fix(R11): close 12 design-to-code convergence gaps from full QA round`).

The patch keeps unresolved product/design issues visible. It does not implement a partial fake Settings roles API, and it does not hide MEM02 extraction timeout or startup security events. Admin Users and same-origin Agent proxy were observed as failed/missing in the raw fourth-round evidence, but R11 now contains source-level implementations for those surfaces; they should be treated as post-R11 rerun items, not as currently missing endpoint claims from this PR guide.

## First Reading Order

1. This file.
2. `src/server/http_server.cpp`
3. `src/server/bootstrap.cpp`
4. `src/llm/openai_client.cpp`
5. `src/llm/http_transport.cpp`
6. `include/cortrix/llm/http_transport.h`
7. `cortrix-mcp/src/cortrix_mcp/tools/core.py`
8. `cortrix-mcp/src/cortrix_mcp/tools/memory.py`
9. `cortrix-mcp/src/cortrix_mcp/tools/new.py`
10. `cortrix-mcp/src/cortrix_mcp/tools/admin.py`
11. `sdk/python/cortrix/resources/memory.py`
12. Web UI changes under `web/src/`
13. Tests under `cortrix-mcp/tests/`, `sdk/python/tests/`, `web/src/store/`, and `tests/unit/`

## Changed Areas

### MCP and SDK contract alignment

- `cortrix_document_status` now sends `namespace` for flat document status lookup.
- `cortrix_batch_submit` now sends `options.async` and `options.on_duplicate` according to the backend contract.
- MCP admin import filter now treats object filters as valid and string filters as expected invalid input.
- MCP `cortrix_memory_extract` converts message arrays into backend `content` input.
- Python SDK `memory.extract` now sends `query_text` and `response_text` instead of stale `query` / `response` keys.

Review notes:

- The wrapper fixes are compatibility repairs against the current backend contract.
- MEM02 extraction itself still fails through the backend and remains a review item.

### Namespace and Memory UI contract

- The Web app resolves the current namespace from backend state before upload, chat, and memory requests.
- Upload, Chat, and Memory stores no longer call backend with stale `default` namespace after clean-room load.
- Memory UI list/create now sends backend-supported `namespace`, `limit`, and `offset`, and normalizes `{memories,total}` into UI pagination.

Review notes:

- This fixes the clean-room UI blocker chain without inventing a new namespace model.
- UI Settings roles and Admin Users are intentionally left blocked because their APIs are product/design decisions.

### OpenAPI split assets and local docs

- Backend exposes split OpenAPI YAML assets under `/paths/*.yaml` and `/components/*.yaml`.
- Vite dev proxy now forwards both `/paths` and `/components` to backend.
- `api/paths/documents.yaml` is aligned with the documented document status behavior used by MCP.

Review notes:

- Runtime root `/docs` and root `/openapi.*` remain the current docs contract.
- Stale `/api/v1/docs` and `/api/v1/openapi.*` expectations should be reviewed as test/design drift, not silently added unless R&D wants aliases.

### Clean-room data-dir startup

- Server bootstrap now creates configured `namespace.data_dir` before namespace/platform DB initialization.

Review notes:

- This removes the clean-room requirement to manually pre-create the runtime data directory.
- It is a startup hardening fix, not a storage architecture change.

### LLM transport observability

- HTTP transport now carries low-level `transport_error` detail.
- OpenAI-compatible client includes transport failure detail when available.
- Unit coverage was added for transport-error detail propagation.

Review notes:

- This improves backend failure diagnosis but does not close MEM02 extraction timeout.
- R&D should decide whether MEM02 needs additional structured diagnostics above this transport layer.

### Static Web UI security headers

- Backend static Web UI serving now sets `Content-Security-Policy`, `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, and `Referrer-Policy: no-referrer`.
- Invalid `frame-ancestors` was removed from the HTML meta CSP because browsers ignore that directive in meta delivery.

Review notes:

- This closes the frontend CSP console warning and static Web smoke gap.
- Startup secret logging and LogSanitizer disabled remain open security events.

## Fourth-round Evidence

Canonical evidence root:

`CortrixGTM/project-ops/cortrix-runtime-validation/20260620-agent-black-box-rd-refresh-repair/`

Primary files:

- `human-review.md`
- `repair/round-01/repair-log.md`
- `repair/round-01/test-results-matrix.md`
- `repair/round-01/afterfix/ui-v5/report.md`
- `repair/round-01/afterfix/api-v2/api-smoke-report.md`
- `repair/round-01/afterfix/mcp-sdk-v2/mcp-real-httpx-rerun/report.md`
- `repair/round-01/afterfix/static-web-smoke.md`

Latest direct afterfix3 result:

| Area | Result |
|---|---:|
| UI routes | 10 pass / 2 blocked / 0 fail |
| API smoke | 20 direct pass + 4 expected-negative pass / 2 blocked / 0 fail |
| MCP real-httpx | 26 pass / 1 blocked / 0 fail |
| Static Web security smoke | pass |
| Startup events | 4 open events |

## Verification Already Run

Recorded in the evidence root:

```text
npm run build
cmake --build build --target cortrix-server
node project-ops/cortrix-runtime-validation/20260620-agent-black-box-rd-refresh-repair/repair/round-01/afterfix/ui-v5/ui-round01-probe.mjs
node project-ops/cortrix-runtime-validation/20260620-agent-black-box-rd-refresh-repair/repair/round-01/afterfix/api-v2/api-smoke.mjs
/Users/scott/Projects/Codex/cortrix/cortrix-agent/venv/bin/python project-ops/cortrix-runtime-validation/20260620-agent-black-box-rd-refresh-repair/repair/round-01/afterfix/mcp-sdk-v2/mcp-real-httpx-probe.py
curl -sS -I http://localhost:18080/
curl -sS -D - http://localhost:18080/namespaces -o /private/tmp/cortrix-static-namespaces-body.html
```

After rebasing this branch onto `origin/main` at `8eb5b6a`, the PR submission step also ran:

```text
git diff --check origin/main..HEAD
npm run build
npm test -- src/store/useAppStore.test.ts
cmake --build build --target cortrix-server
./build/tests/cortrix_unit_tests '--gtest_filter=OpenAiLlmClientTest.*'
```

Follow-up Python target retest after installing dependencies into an isolated Python 3.12 venv:

```text
/opt/homebrew/bin/python3.12 -m venv /private/tmp/cortrix-r4-python-target-venv
/private/tmp/cortrix-r4-python-target-venv/bin/python -m pip install -e '/Users/scott/Projects/Codex/cortrix/cortrix-mcp[test]' -e '/Users/scott/Projects/Codex/cortrix/sdk/python[dev]'
cd /Users/scott/Projects/Codex/cortrix/cortrix-mcp
/private/tmp/cortrix-r4-python-target-venv/bin/python -m pytest -p no:cacheprovider tests/test_core.py tests/test_admin.py
cd /Users/scott/Projects/Codex/cortrix/sdk/python
/private/tmp/cortrix-r4-python-target-venv/bin/python -m pytest -p no:cacheprovider tests/test_memory.py tests/test_async.py
```

Results:

- Frontend production build passed.
- Frontend `useAppStore` test passed: 8 tests.
- C++ server build passed.
- C++ `OpenAiLlmClientTest` passed: 9 tests.
- Python MCP target retest passed: 60 tests.
- Python SDK target retest passed: 19 tests.

This PR submission did not rerun the full black-box suite; it packages the already-recorded fourth-round result plus the targeted post-rebase and Python target retest checks above. Because R11 landed before PR submission, some raw fourth-round failures must be reclassified by a clean-room post-R11 rerun.

## Remaining Blockers Not Solved In This PR

| ID | Surface | Current status after R11 rebase | Why it remains open |
|---|---|---|---|
| `RB-MEM02-CPP-HTTPLIB-GLM-CONNECTION` | API/MCP memory extract | Raw fourth-round evidence hit `CX_ERR_MEM02_EXTRACT_LLM_TIMEOUT`; this PR adds transport detail only. | Wrapper and transport repairs did not prove backend MEM02 extraction closed. Requires rerun with current R11+PR branch and GLM config. |
| `RB-SETTINGS-ROLES-CONTRACT-DRIFT` | UI Settings / Agent config | Still visible in source: `SettingsPage` calls Agent `/config/llm/roles`, while canonical server-side config is `/api/v1/system/agent_llm_config` and the dialog calls Agent `/config/agent_llm`. | R&D should decide whether Settings roles should be removed, hidden, mapped to `agent_llm_config`, or restored as an Agent API. |
| `RB-ADMIN-USERS-R11-RERUN-GATE` | UI Admin Users | Raw fourth-round evidence saw `/api/v1/admin/users` 404; R11 now adds the P08 admin/users endpoint set and `UsersPage`. | No clean-room post-R11 UI/API rerun has been recorded in this PR submission. Treat as verification-required, not currently missing. |
| `RB-AGENT-PROXY-R11-RERUN-GATE` | UI Agent / same-origin backend Agent proxy | Earlier black-box evidence saw backend `/api/v1/agent/*` 404; R11 now registers `RegisterAgentProxyRoutes`. | No clean-room post-R11 SSE/auth/proxy rerun has been recorded in this PR submission. Treat as verification-required. |
| `RB-SECURITY-STARTUP-EXPOSURE` | Runtime startup | Source still prints the bootstrap token URL and initializes LogSanitizer from a fixed `/app/config/sensitive_fields.yaml` path. | Preserved as raw security evidence; R&D should decide local-dev versus production logging policy. |
| `RB-AUTH-BOOTSTRAP-PORT-MISMATCH` | Runtime startup | Source still prints `localhost:8420` in the bootstrap URL. | Active backend port can differ; decide whether to render configured host/port. |
| `RB-TASKS-METADATA-DUPLICATE-COLUMN` | Runtime startup/migration | Task metadata migration still uses an unconditional `ALTER TABLE tasks ADD COLUMN metadata_json TEXT`. | The raw warning may persist on existing DBs; core smoke was not blocked, but startup remains noisy. |

## R&D Decisions Requested

1. Accept, replace, or split the MCP/SDK/UI/backend compatibility fixes in this PR.
2. Decide whether MEM02 timeout blocks merge or remains a known backend issue.
3. Decide whether Settings roles should be removed, hidden, re-mapped to `agent_llm_config`, or restored as an Agent API.
4. Decide whether R11's Admin Users and Agent proxy implementations satisfy the prior black-box blockers after a clean-room rerun.
5. Decide whether `/api/v1/system/status`, `/api/v1/system/stats`, `/api/v1/docs`, and `/api/v1/openapi.*` should stay stale expectations or become aliases.
6. Decide how to handle startup token logging, LogSanitizer config absence, bootstrap port mismatch, and duplicate migration warning.

## Files Intentionally Not Included

The local working tree also contains untracked `benchmarks/` files. They are not part of this PR and should not be reviewed as part of the Round-4 repair patch.

## Boundary

Raw evidence is intentionally preserved 1:1 in the private CortrixGTM evidence root. This source PR summarizes and links that evidence; it does not sanitize or replace it.
