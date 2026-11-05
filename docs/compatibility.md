# Compatibility And Known Status

This page defines the public status language for Cortrix docs.

Cortrix is in active pre-release development. Nothing on this page is a production-readiness commitment. Some API surfaces are present in the OpenAPI spec but are still in development.

## Status Labels

| Status | Meaning |
|---|---|
| `Preview` | Available and documented. Cortrix is in pre-release, so verify it against your own runtime before production use. |
| `In development` | Present in part or being built; not supported yet. Do not rely on it. |
| `Not provided` | The server in this repository does not serve it on this release line. Do not rely on it. |

## Current Status Matrix

| Area | Status | Public guidance |
|---|---|---|
| OpenAPI spec file | `Preview` | `api/openapi.yaml` is the canonical public contract file. |
| Local health endpoints | `Preview` | Use them for local checks; do not infer full production readiness from health alone. |
| Docker Quick Start | `Preview` | The Compose deployment builds from source, downloads the pinned local models, and serves the loopback API; it is for local evaluation, without authentication. |
| Namespaces, documents, and query | `Preview` | Core surfaces exist and are documented; verify against your runtime before production use. |
| Directory watchers | `Preview` | Adding, listing, and removing a watcher are documented; removing a watcher also purges the documents it imported. `GET /watch/{id}/events` is not implemented on this line and returns 404. |
| Explicit memory write, list, edit, and delete | `Preview` | The four endpoints exist and are documented. Read explicit memories back with `GET /memory`; semantic search over them (`POST /memory/search` and `/query`) does not return results on this line. |
| Self-service memory correction | `Preview` | The manual invalidate and restore flow and its audit trail are documented in [Agent Memory Correction](agent-memory-correction.md) and present; automatic contradiction handling depends on memory extraction, which is `In development`. |
| MCP server | `Preview` | The local stdio adapter supports modern `2026-07-28` (`server/discover`) and legacy `2025-11-25` (initialize); verify it against your target server and API. |
| Python SDK | `Preview` | The typed models are generated from the published specification, and CI fails when the two drift. The resource methods are tested against mocked responses rather than a live server, so compatibility follows the live API contract. |
| Framework adapters (`cortrix-skills`) | `Preview` | Tool definitions for LangChain, Claude, and OpenAI install from PyPI and are test-covered; a full round trip depends on your LLM provider. |
| pgcortrix | `In development` | The SQL contract and helper exist with standalone tests; loading into a live PostgreSQL and `pg_regress` integration have not been run on this line. |
| Built-in Agent fixed-flow chat | `Preview` | Chat mode is documented; deployment and LLM provider behavior should be verified in your runtime. |
| Account login (email and password) | `Not provided` | The server authenticates with API keys. `POST /auth/register`, `/auth/login`, `/auth/logout`, `/auth/refresh`, `/auth/password-reset`, `/auth/password-reset/confirm`, and `/auth/verify-email` return 404. In the Python SDK, `client.auth.register`, `login`, `logout`, `refresh`, and `password_reset` raise `FeatureNotAvailableError` against this server; `client.auth.me` works. See [Auth and security boundaries](#auth-and-security-boundaries). |
| Tenant/member/ACL/quota | `In development` | Not supported yet. |
| RBAC | `In development` | Not supported yet. |
| Tenant isolation | `In development` | Not supported yet. |
| Memory extraction | `In development` | Not supported yet. |
| OCR / parser paths | `Preview` | Parser and OCR behavior depends on optional configuration and should be verified per deployment. |
| Linux NVIDIA CUDA execution provider | `Preview` | A separate Linux x86_64 image and runbook exist; run a platform capability smoke test in your target deployment. |
| Log redaction / LogSanitizer defaults | `In development` | Not supported yet. |
| Database import | `In development` | Not supported yet. |

## Auth And Security Boundaries

The OpenAPI spec defines both API key and Bearer auth schemes. In local development, auth may be disabled. Do not use an auth-disabled local runtime to claim RBAC, tenant isolation, ACL, or quota enforcement.

Tenant/member/ACL/quota, RBAC, tenant isolation, and log redaction defaults are `In development`. Until those capabilities are available, deploy Cortrix only on a network you control.

This server authenticates with API keys and does not serve email and password login. With `auth.enabled: false`, as set in `config.yaml.example` and `deploy/cortrix.yaml` (the built-in default without a config file is `true`; it is accepted only on a loopback `server.host` or an explicit container-internal bind published only to loopback), requests need no key. With auth enabled, the first key comes from an `auth.api_keys` entry in the config (see `config.yaml.example`; `permissions: 7` includes ADMIN), or from the one-time setup URL the server prints on first start. That URL is valid for 60 seconds, works once, and accepts loopback clients only by default; opening it shows an admin API key. Create further keys with `POST /api/v1/auth/api-keys` using the admin key from the setup URL. Requests without an admin key are rejected.

### Namespace not found vs. not authorized

With API-key authentication enabled, a request naming a namespace the key cannot read, including a namespace that does not exist, returns HTTP 403 with `error.code` set to `CX_ERR_NS_UNAUTHORIZED` and `error.category` set to `auth`. This is intentional: the response does not reveal which namespaces exist.

This namespace check applies at every key permission level, including keys with admin permissions. It covers single-namespace routes such as `GET /api/v1/namespaces/{name}` and `GET /api/v1/namespaces/{name}/documents`, as well as `POST /api/v1/query`.

For a cross-namespace query, any failed namespace check rejects the whole request. `error.structured_data.unauthorized_namespaces` lists the requested names that failed the check, which can include both namespaces the key cannot read and namespaces that do not exist. The list only echoes names the caller sent.

Agents should not retry this error with a different or higher-permission key. Call `GET /api/v1/namespaces` with the same key first; it lists only namespaces that key can read. A name missing from that list may be nonexistent or unreadable; the list does not prove nonexistence.

Treat a missing name as a wrong or missing namespace first: fix the name, or create it with `POST /api/v1/namespaces` if that is the intent and the key has admin permission. Only then consider access permissions.

With auth disabled (the Quick Start's loopback default), this namespace authorization rule does not apply. A missing namespace on `GET /api/v1/namespaces/{name}` returns HTTP 404 with `error.code` set to `CX_ERR_NS_NOT_FOUND`. Cross-namespace queries instead report missing namespaces in `meta.namespaces_failed` in an HTTP 200 response, as shown in the [partial success example](../examples/partial-success/README.md).

## API Compatibility Boundaries

OpenAPI presence means an endpoint is part of the documented API surface. It does not prove that the endpoint is verified in the current runtime.

When writing client code:

- prefer the OpenAPI spec for request and response shapes;
- check this status page for areas that are still in development;
- treat `Preview` surfaces as integration candidates, not production-readiness guarantees;
- pin your target server version and rerun smoke tests after updating Cortrix.

## Agent Integration Boundaries

MCP, Python SDK, and the built-in Agent are all documented access paths. They are not interchangeable:

- MCP exposes Cortrix operations as MCP tools over local stdio. It supports
  modern `2026-07-28` discovery and the legacy `2025-11-25` initialize path;
  no remote Streamable HTTP MCP endpoint is a current capability.
- The Python SDK exposes Cortrix resources to Python applications.
- The built-in Agent exposes a fixed-flow chat service over FastAPI.

The built-in Agent offers fixed-flow chat only.

## Benchmark Boundary

Published benchmark numbers must link to immutable measured artifacts and methodology. The current accepted scope is the pinned [four-corpus CPU measurement bundle](https://github.com/cortrix/cortrix-benchmarks/tree/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1): full-corpus SciFact, NFCorpus, and FiQA with every judged test query, plus full-corpus Quora with the first 2,000 of 10,000 judged queries. It was measured against Core `79a4eb17c62521338d1ac47a9749e6230e87e69b` with public runner commit `9490520c24a96ed97b80073ed3ebab096b80550b`.

The bundle measures retrieval quality at `top_k=10`. It is not evidence of end-to-end answer quality, concurrent production latency or capacity, security or compliance properties, competitive ranking, or business outcomes. Comparisons across independently ingested namespaces carry the bundle's measured variation floor; Quora is the only strictly controlled shared-namespace arm comparison.

## Production Boundary

Before production use, verify at least:

- auth mode and credential handling;
- tenant isolation and RBAC behavior;
- namespace ACLs and quotas;
- memory extraction behavior;
- logging and redaction;
- data persistence and backup strategy;
- deployment topology and resource limits;
- API/SDK/MCP compatibility against your target build.

Treat any item you have not verified as not ready.
