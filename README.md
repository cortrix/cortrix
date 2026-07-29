<div align="center">
  <img src="docs/assets/cortrix-logo.svg" width="100" alt="Cortrix logo">

  # Cortrix

  Agent-native semantic storage for retrieval, memory, and API-driven AI applications.

  [![License: AGPL-3.0-only](https://img.shields.io/badge/License-AGPL--3.0--only-blue.svg)](LICENSE)
  [![Version](https://img.shields.io/badge/version-v1.0.0--rc.1-orange.svg)](https://github.com/cortrix/cortrix/releases)
  [![C++17](https://img.shields.io/badge/C%2B%2B-17-blueviolet.svg)](https://en.cppreference.com/w/cpp/17)

  [Quickstart](docs/QUICKSTART.md) · [Install with an AI Agent](docs/AGENT_QUICKSTART.md) · [Agent access](docs/agent-access.md) · [Compatibility](docs/compatibility.md) · [OpenAPI](api/openapi.yaml)
</div>

Cortrix provides a local-first semantic storage server with namespaces, documents, blocks, hybrid search, memory APIs, and Agent-oriented access paths. It is designed for applications and Agents that need a programmable retrieval layer rather than a one-off vector database wrapper.

## Architecture

<p align="center">
  <img src="https://cortrix.ai/assets/architecture.svg" width="963" alt="Cortrix architecture overview with source-processing, retrieval, record, storage, and integration paths labeled by evidence boundary">
</p>

The repository includes:

- `cortrix-server`: the C++ backend and HTTP API.
- `api/openapi.yaml`: the public OpenAPI contract.
- `cortrix-mcp/`: an MCP server for IDE and Agent clients.
- `sdk/python/`: the Python SDK.
- `cortrix-agent/`: the built-in fixed-flow RAG chat service.
- `web/`: the local Web UI.

## Documentation Map

Start here:

- [Quickstart](docs/QUICKSTART.md): start Cortrix with Docker, check readiness, and run a source-backed reranked query.
- [Install with an AI Agent](docs/AGENT_QUICKSTART.md): give a terminal-capable AI Agent a version-pinned local setup and verification contract.
- [First-value SupportOps demo](examples/first-value-supportops/README.md): run the deeper source-build verification path with versioned assertions, trace evidence, and zero-residual cleanup.
- [Stack fit and adoption boundaries](docs/adoption/stack-fit.md): review evidence-backed keep/add/replace/unknown decision cards.
- [Agent access](docs/agent-access.md): choose between HTTP/OpenAPI, MCP, Python SDK, and the built-in Agent.
- [Compatibility and known status](docs/compatibility.md): current public status for API, MCP, SDK, Agent, auth, tenant/RBAC, memory extraction, benchmarks, and security hardening.
- [OpenAPI spec](api/openapi.yaml): endpoint paths, schemas, security schemes, and response contracts.
- [MCP README](cortrix-mcp/README.md): MCP server setup and tool reference.
- [Python SDK README](sdk/python/README.md): Python client setup and resource model.
- [Built-in Agent README](cortrix-agent/README.md): FastAPI Agent service and chat endpoints.
- [Linux NVIDIA CUDA operations](docs/operations/cuda-execution-provider.md):
  separate image, provider policy, deployment, verification, and rollback.
- [Security policy](SECURITY.md): security reporting path.
- [Contributing](CONTRIBUTING.md): development workflow and contribution checklist.
- [Maintainers](MAINTAINERS.md): ownership, acknowledgment targets, and recusal rules.
- [Code of Conduct](CODE_OF_CONDUCT.md): community standards and private reporting.

## Current Status

Cortrix is in active pre-release development. The public documentation uses these status labels:

- `Verified`: directly supported by current code/spec evidence and exercised in the latest validation scope.
- `Verification required`: present in code, spec, or docs, but not yet confirmed enough for a public-readiness claim.
- `Blocked`: known broken, blocked, or not provable in the current runtime.
- `Roadmap`: planned or reserved for a later version.

High-signal current status:

| Area | Status | Notes |
|---|---|---|
| OpenAPI file | `Verified` | `api/openapi.yaml` is present and declares the public API surface. |
| Local server, health endpoints, namespaces, documents, query | `Verification required` | Core surfaces exist, but public-readiness labeling still depends on end-to-end verification. |
| MCP server | `Verification required` | MCP tooling exists and has test coverage, with release-readiness still under review. |
| Python SDK | `Verification required` | SDK resources and tests exist, with compatibility still tied to the live API contract. |
| Built-in Agent chat | `Verification required` | Fixed-flow chat mode exists; advanced autonomous executors are roadmap items. |
| Auth login | `Blocked` | The public spec defines login, but the latest runtime verification found contract drift. |
| Tenant/member/ACL/quota | `Blocked` | Runtime behavior and documented contract are still being reconciled. |
| MEM02 memory extraction | `Blocked` | Latest verification observed an LLM transport timeout path. |
| RBAC and tenant isolation denial matrix | `Blocked` | Cannot be proven in the current auth-disabled local runtime. |
| Full-corpus BEIR retrieval quality | `Verified` | Accepted SciFact, FiQA, and NFCorpus measurements, method, and provenance are published in the [pinned benchmark bundle](https://github.com/cortrix/cortrix-benchmarks/tree/7bc29aa840c20db3935dfcf80eb048e553ebe2b0/results/published/beir-three-full-corpus-2026-07-v1). This does not establish answer quality or production performance. |

See [Compatibility and known status](docs/compatibility.md) before making production, security, benchmark, or integration claims.

## Quickstart

Run the local Docker Quick Start with real embedding and reranking:

```bash
git clone https://github.com/cortrix/cortrix.git
cd cortrix
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait

curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready

curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["demo"],
    "query": "What does semantic storage keep close to the agents that need it?",
    "top_k": 5,
    "rerank": true
  }' \
  http://127.0.0.1:8420/api/v1/query
```

You need Git, Docker, and Docker Compose. No `.env` file, LLM provider key, host-side model tooling, manual model download, model conversion, or separate bootstrap command is required. The first start downloads about 1.17 GB of pinned model assets and can take several minutes; later starts reuse the cached volume.

The Quick Start publishes only the loopback API at `127.0.0.1:8420`. It uses BGE-M3 embedding and bge-reranker-v2-m3 reranking on CPU, while external LLM roles and the built-in Agent remain disabled. For model provenance, checks, expected output, cleanup, and scope boundaries, see [Quick Start](docs/QUICKSTART.md). For the deeper source-build evidence workflow, see the [First-value SupportOps demo](examples/first-value-supportops/README.md).

If you are using a terminal-capable AI Agent, use the
[Agent-assisted setup contract](docs/AGENT_QUICKSTART.md). It pins the release
and commit, limits the agent to the same loopback-only Docker path, and requires
a structured verification report.

## API Reference

The canonical API contract is [api/openapi.yaml](api/openapi.yaml).

The spec includes:

- local and cloud server URLs;
- API key and Bearer auth schemes;
- namespaces, documents, query, memory, watch, import, auth, admin, tenant, Agent, system, GC, and maintenance paths;
- request/response schemas and error schemas.

OpenAPI presence is not the same as runtime verification. If an endpoint is listed as `Verification required` or `Blocked` in [Compatibility](docs/compatibility.md), use that status as the public claim boundary.

## Agent Access

Cortrix exposes four access paths:

| Path | Best for | Entry point |
|---|---|---|
| HTTP API / OpenAPI | Custom services and direct Agent calls | [api/openapi.yaml](api/openapi.yaml) |
| MCP server | IDE Agents and MCP-compatible clients | [cortrix-mcp/README.md](cortrix-mcp/README.md) |
| Python SDK | Python applications and RAG pipelines | [sdk/python/README.md](sdk/python/README.md) |
| Built-in Agent | Local fixed-flow chat over Cortrix storage | [cortrix-agent/README.md](cortrix-agent/README.md) |

See [Agent access](docs/agent-access.md) for selection guidance and current support status.

## Core Concepts

- **Namespace**: a logical collection boundary for documents, blocks, memory, and queries.
- **Document**: uploaded source material that can be parsed and indexed.
- **Block**: a searchable unit derived from a document or memory record.
- **Query**: a retrieval request over one or more namespaces.
- **Memory**: structured long-term information captured from interactions or explicit API calls.
- **Agent surface**: a programmatic path that lets an Agent use Cortrix through HTTP, MCP, SDK, or the built-in Agent service.

## Configuration

The default local config template is `config.yaml.example`. Copy it into `build/config.yaml` before the first run:

```bash
cp config.yaml.example build/config.yaml
```

LLM-backed features are configured by role:

- `semantic_llm`: optional LLM-backed semantic processing. It is separate from the local ONNX embedding and cross-encoder reranker used by the primary Quick Start.
- `vision_llm`: OCR image enhancement.
- `agent_llm`: built-in Agent chat.
- `doc_summary_llm`: ingest-side summaries.
- `enricher_llm`: ingest enrichment.

Use placeholder values in documentation and real provider keys only in local ignored config files. Do not commit secrets.

## Production And Security Notes

Cortrix is pre-release. Before using it outside local development, review:

- [Compatibility and known status](docs/compatibility.md) for blocked and review-required areas.
- [Security policy](SECURITY.md) for reporting.
- `config.yaml.example` for auth and API key configuration.
- `deploy/` for deployment templates.

Do not assume production readiness for auth, tenant isolation, RBAC, quota enforcement, or memory extraction. Do not generalize the published retrieval-quality measurements to answer quality, latency, cost, security, or production performance.

## Roadmap

Roadmap items are not current capabilities.

- Advanced autonomous Agent executors such as tool-use and plan-execute modes.
- Additional integration adapters beyond the currently documented surfaces.
- A remote MCP access path using stateless Streamable HTTP, with production-grade
  authentication, authorization, and deployment hardening. The current MCP
  capability is local stdio only.
- Production-readiness hardening for auth, tenant isolation, RBAC, quota, logging redaction, and deployment operations.
- Additional datasets and production-representative latency or cost measurements after separate methods and artifacts are accepted.

## Community And Contribution

- [GitHub Issues](https://github.com/cortrix/cortrix/issues): bugs and feature requests.
- [Issue templates](https://github.com/cortrix/cortrix/issues/new/choose): bugs, feature or integration proposals, and documentation reports.
- [Security policy](SECURITY.md): security reports.
- [Contributing](CONTRIBUTING.md): local development and pull requests.
- [Code of Conduct](CODE_OF_CONDUCT.md): community standards and private conduct reports.
- [Maintainers](MAINTAINERS.md): ownership and acknowledgment targets.

## License

Cortrix-authored material is licensed under [AGPL-3.0-only](LICENSE); third-party material retains its own license. See [NOTICE.md](NOTICE.md) for copyright and exception boundaries and [CONTRIBUTING.md](CONTRIBUTING.md) for the no-CLA, DCO 1.1 contribution policy.
