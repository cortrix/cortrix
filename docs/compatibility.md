# Compatibility And Known Status

This page defines the public status language for Cortrix docs.

Cortrix is in active pre-release development. Some API surfaces are present in the OpenAPI spec but are not yet safe to describe as verified runtime behavior.

## Status Labels

| Status | Meaning |
|---|---|
| `Verified` | Directly supported by current code/spec evidence and exercised in the latest validation scope. |
| `RD review required` | Present in code, spec, or docs, but not yet confirmed enough for a public-readiness claim. |
| `Blocked` | Known broken, blocked, or not provable in the current runtime. |
| `Roadmap` | Planned or reserved for a later version. |

## Current Status Matrix

| Area | Status | Public guidance |
|---|---|---|
| OpenAPI spec file | `Verified` | `api/openapi.yaml` is the canonical public contract file. |
| Local health endpoints | `RD review required` | Use them for local checks; do not infer full production readiness from health alone. |
| Namespaces, documents, and query | `RD review required` | Core surfaces exist and are documented; verify against your runtime before production use. |
| MCP server | `RD review required` | MCP tools are documented and test-covered, but public readiness still depends on server/API compatibility. |
| Python SDK | `RD review required` | SDK resources are documented and test-covered, but compatibility follows the live API contract. |
| Built-in Agent fixed-flow chat | `RD review required` | Chat mode is documented; deployment and LLM provider behavior should be verified in your runtime. |
| Built-in Agent tool-use and plan-execute modes | `Roadmap` | These executor modes are not current production capabilities. |
| Auth login | `Blocked` | The spec defines login, but latest runtime verification found contract drift. |
| Tenant/member/ACL/quota | `Blocked` | Runtime behavior and documented request/response contracts are still being reconciled. |
| RBAC deny matrix | `Blocked` | Denial behavior cannot be proven in the current auth-disabled local runtime. |
| Tenant isolation deny matrix | `Blocked` | Denial behavior cannot be proven in the current auth-disabled local runtime. |
| MEM02 memory extraction | `Blocked` | The latest verification found an LLM transport timeout path for memory extraction. |
| OCR / parser paths | `RD review required` | Parser and OCR behavior depends on optional configuration and should be verified per deployment. |
| Log redaction / LogSanitizer defaults | `Blocked` | Runtime hardening is still under review; do not claim sanitized startup logs by default. |
| Benchmark results | `RD review required` | Do not publish benchmark claims until measured artifacts and methodology are accepted. |
| Production readiness | `RD review required` | Treat deployment, auth, tenant isolation, limits, and security controls as review-required. |

## Auth And Security Boundaries

The OpenAPI spec defines both API key and Bearer auth schemes. In local development, auth may be disabled. Do not use an auth-disabled local runtime to claim RBAC, tenant isolation, ACL, or quota enforcement.

Known public boundary:

- API key configuration exists in the docs and config templates.
- Auth login is currently `Blocked`.
- Tenant/member/ACL/quota contract conformance is currently `Blocked`.
- RBAC and tenant isolation denial matrices are currently `Blocked`.
- Log redaction defaults are currently `Blocked`.

## API Compatibility Boundaries

OpenAPI presence means an endpoint is part of the documented API surface. It does not prove that the endpoint is verified in the current runtime.

When writing client code:

- prefer the OpenAPI spec for request and response shapes;
- check this status page for known blocked areas;
- treat `RD review required` surfaces as integration candidates, not public readiness guarantees;
- pin your target server version and rerun smoke tests after updating Cortrix.

## Agent Integration Boundaries

MCP, Python SDK, and the built-in Agent are all documented access paths. They are not interchangeable:

- MCP exposes Cortrix operations as MCP tools over stdio.
- The Python SDK exposes Cortrix resources to Python applications.
- The built-in Agent exposes a fixed-flow chat service over FastAPI.

The built-in Agent's advanced autonomous executors are roadmap items.

## Benchmark Boundary

Do not publish benchmark numbers from README, slides, or external materials unless they link to accepted measured artifacts and methodology.

Until that review is complete, benchmark content should be described as `RD review required`.

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

If any item is not verified, document it as `RD review required` or `Blocked`.
