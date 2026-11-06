# Compatibility And Known Status

This page defines the public status language for Cortrix docs.

Cortrix is in active pre-release development. Nothing on this page is a production-readiness commitment. Some API surfaces are present in the OpenAPI spec but are still in development.

## Status Labels

| Status | Meaning |
|---|---|
| `Preview` | Available and documented. Cortrix is in pre-release, so verify it against your own runtime before production use. |
| `Off by default` | Implemented, but not active in the shipped configuration (`auth.enabled: false`). Turn on authentication to use it, and read what happens while it is off. |
| `In development` | Present in part or being built; not supported yet. Do not rely on it. |

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
| Python SDK | `Preview` | SDK resources are documented and test-covered, but compatibility follows the live API contract. |
| Framework adapters (`cortrix-skills`) | `Preview` | Tool definitions for LangChain, Claude, and OpenAI install from PyPI and are test-covered; a full round trip depends on your LLM provider. |
| pgcortrix | `In development` | The SQL contract and helper exist with standalone tests; loading into a live PostgreSQL and `pg_regress` integration have not been run on this line. |
| Built-in Agent fixed-flow chat | `Preview` | Chat mode is documented; deployment and LLM provider behavior should be verified in your runtime. |
| Auth login | `In development` | Not supported yet. |
| Tenants, members, and namespace ACLs | `Off by default` | The tenant and member endpoints are always served. Namespace ACLs are checked only with `auth.enabled: true`; with auth off, every request acts as an admin of `default_tenant`, including on these endpoints. See [Auth and security boundaries](#auth-and-security-boundaries). |
| Tenant isolation | `Off by default` | Enforced only with `auth.enabled: true`. With auth off, every request runs in `default_tenant` and can read every namespace. |
| API key permissions (READ, WRITE, ADMIN) | `Off by default` | Checked on every request with `auth.enabled: true`. With auth off, every request has all three. |
| Roles (RBAC) | `In development` | Roles are not implemented; access is decided by a key's permission bits. |
| Quotas | `In development` | Quota values can be read and updated through the tenant endpoints, but requests are not checked against them. |
| Memory extraction | `In development` | Not supported yet. |
| OCR / parser paths | `Preview` | Parser and OCR behavior depends on optional configuration and should be verified per deployment. |
| Linux NVIDIA CUDA execution provider | `Preview` | A separate Linux x86_64 image and runbook exist; run a platform capability smoke test in your target deployment. |
| Log redaction / LogSanitizer defaults | `In development` | Not supported yet. |
| Database import | `In development` | Not supported yet. |

## Auth And Security Boundaries

The OpenAPI spec defines both API key and Bearer auth schemes. In local development, auth may be disabled. Do not use an auth-disabled local runtime to claim RBAC, tenant isolation, ACL, or quota enforcement.

Tenant isolation, namespace ACLs, and API key permissions are `Off by default`: the server enforces them only with `auth.enabled: true`. With auth off, every request is treated as an admin of `default_tenant`. It can read and write every namespace, call the tenant, member, and maintenance endpoints, and restore or purge deleted documents. The server accepts auth off only on a loopback `server.host` or an explicit container-internal bind published only to loopback, so anyone who can reach that port has full access. Auth login, roles, quotas, and log redaction defaults are `In development`. API key configuration exists in the docs and config templates. Until those are available, deploy Cortrix only on a network you control.

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
