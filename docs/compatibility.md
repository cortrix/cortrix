# Compatibility And Known Status

This page defines the public status language for Cortrix docs.

Cortrix is in active pre-release development. Nothing on this page is a production-readiness commitment. Some API surfaces are present in the OpenAPI spec but are still in development.

## Status Labels

| Status | Meaning |
|---|---|
| `Preview` | Available and documented. Cortrix is in pre-release, so verify it against your own runtime before production use. |
| `In development` | Being built and not available yet. |
| `Planned` | Reserved for a later version. |

## Current Status Matrix

| Area | Status | Public guidance |
|---|---|---|
| OpenAPI spec file | `Preview` | `api/openapi.yaml` is the canonical public contract file. |
| Local health endpoints | `Preview` | Use them for local checks; do not infer full production readiness from health alone. |
| Namespaces, documents, and query | `Preview` | Core surfaces exist and are documented; verify against your runtime before production use. |
| MCP server | `Preview` | The local stdio adapter supports modern `2026-07-28` (`server/discover`) and legacy `2025-11-25` (initialize); verify it against your target server and API. |
| Python SDK | `Preview` | SDK resources are documented and test-covered, but compatibility follows the live API contract. |
| Built-in Agent fixed-flow chat | `Preview` | Chat mode is documented; deployment and LLM provider behavior should be verified in your runtime. |
| Built-in Agent tool-use and plan-execute modes | `Planned` | These executor modes are not current production capabilities. |
| Auth login | `In development` | Not available yet. |
| Tenant/member/ACL/quota | `In development` | Not available yet. |
| RBAC | `In development` | Not available yet. |
| Tenant isolation | `In development` | Not available yet. |
| MEM02 memory extraction | `In development` | Not available yet. |
| OCR / parser paths | `Preview` | Parser and OCR behavior depends on optional configuration and should be verified per deployment. |
| Linux NVIDIA CUDA execution provider | `Preview` | A separate Linux x86_64 image and runbook exist; run a platform capability smoke test in your target deployment. |
| Log redaction / LogSanitizer defaults | `In development` | Not available yet. |
| Database import | `In development` | Not available yet. |

## Auth And Security Boundaries

The OpenAPI spec defines both API key and Bearer auth schemes. In local development, auth may be disabled. Do not use an auth-disabled local runtime to claim RBAC, tenant isolation, ACL, or quota enforcement.

Auth login, tenant/member/ACL/quota, RBAC, tenant isolation, and log redaction defaults are `In development`. API key configuration exists in the docs and config templates. Until those capabilities are available, deploy Cortrix only on a network you control.

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

The built-in Agent's advanced autonomous executors are `Planned`.

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
