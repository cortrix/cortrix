# Documentation sources

Every documentation page is written from material in this repository. This
file records which files back each page so a claim can be checked against its
source. Paths are relative to the repository root and are read at the
`v1.0.0-rc.2` tag unless a page says otherwise.

Status follows [docs/compatibility.md](../docs/compatibility.md).

## Get started

| Page | Sources | Status |
|---|---|---|
| `get-started/overview` | `README.md` | — |
| `get-started/quickstart` | `docs/QUICKSTART.md`, `deploy/docker-compose.yml`, `deploy/quickstart-bootstrap.sh` | Preview |
| `get-started/agent-quickstart` | `docs/AGENT_QUICKSTART.md` | Preview |
| `get-started/core-concepts` | `README.md`, `api/components/schemas.yaml` | — |

## Guides

| Page | Sources | Status |
|---|---|---|
| `guides/upload-documents` | `api/paths/documents.yaml`, `api/paths/namespaces.yaml`, `sdk/python/README.md` | Preview |
| `guides/query` | `api/paths/query.yaml`, `docs/operations/reranking-applicability.md` | Preview |
| `guides/memory` | `api/paths/memory.yaml`, `docs/agent-memory-correction.md` | Preview; extraction is In development |
| `guides/watch` | `api/paths/watch.yaml` | Preview |
| `guides/database-import` | `api/paths/import.yaml` | Preview |

## Integrations

| Page | Sources | Status |
|---|---|---|
| `integrations/overview` | `docs/agent-access.md` | Preview |
| `integrations/mcp` | `cortrix-mcp/README.md` | Preview |
| `integrations/python-sdk` | `sdk/python/README.md` | Preview |
| `integrations/skills` | `cortrix-skills/README.md` | Preview |
| `integrations/agent` | `cortrix-agent/README.md`, `api/paths/agent.yaml` | Preview |
| `integrations/pgcortrix` | `docs/compatibility.md` | In development; placeholder page |

## Deploy

| Page | Sources | Status |
|---|---|---|
| `deploy/docker-compose` | `deploy/docker-compose.yml`, `deploy/Dockerfile`, `deploy/QUICKSTART.md` | Preview |
| `deploy/configuration` | `config.yaml.example`, `deploy/cortrix.yaml`, `deploy/.env.example` | Preview |
| `deploy/models` | `deploy/MODELS.md`, `deploy/model-manifest.tsv`, `docs/operations/onnx-upgrade.md` | Preview |
| `deploy/performance-tuning` | `docs/operations/reranking-applicability.md`, `config.yaml.example`, `src/config/config.cpp`, `deploy/entrypoint.sh`, `api/components/schemas.yaml` | Preview |
| `deploy/cuda` | `docs/operations/cuda-execution-provider.md`, `deploy/docker-compose.cuda.yml` | Preview |
| `deploy/security` | `docs/compatibility.md`, `SECURITY.md`, `config.yaml.example` | In-development areas stated as such |
| `deploy/upgrade` | `docs/releases/v1.0.0-rc.2-upgrade.md` | — |

## Concepts

| Page | Sources | Status |
|---|---|---|
| `concepts/architecture` | `README.md`, `src/`, `CMakeLists.txt` | — |
| `concepts/data-flow` | `src/`, `api/paths/documents.yaml`, `api/paths/query.yaml` | — |
| `concepts/interfaces` | `api/components/errors.yaml`, `api/components/x-cortrix.yaml`, `docs/agent-access.md` | — |

## Reference

| Page | Sources | Status |
|---|---|---|
| `reference/api` | `api/openapi.yaml`, `api/paths/`, `api/components/` (rendered directly) | Per endpoint |
| `reference/mcp-tools` | `cortrix-mcp/README.md`, `cortrix-mcp/` tool definitions | Preview |
| `reference/configuration` | `config.yaml.example` | — |
| `reference/errors` | `api/components/errors.yaml` | — |
| `reference/glossary` | `README.md`, `api/components/schemas.yaml`, `config.yaml.example`, `docs/feature-index.md` | — |

## Resources

| Page | Sources | Status |
|---|---|---|
| `resources/compatibility` | `docs/compatibility.md` | — |
| `resources/stack-fit` | `docs/adoption/stack-fit.md` | — |
| `resources/benchmarks` | `docs/compatibility.md` (benchmark boundary), linked `cortrix-benchmarks` bundle | Preview |
| `resources/troubleshooting` | `docs/QUICKSTART.md`, `deploy/healthcheck.sh`, component READMEs | — |
| `resources/releases` | `docs/releases/v1.0.0-rc.2.md` | — |
| `resources/contributing` | `CONTRIBUTING.md`, `DCO`, `docs/BRANCHING.md`, `CODE_OF_CONDUCT.md` | — |
