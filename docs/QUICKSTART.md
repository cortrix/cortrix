# Cortrix Quick Start

This guide starts Cortrix with Docker and runs a source-backed local query with real ONNX embedding and reranking. External LLM roles stay disabled, so the run needs no provider key.

Cortrix is in active pre-release development. Read [Compatibility and known status](compatibility.md) before treating any surface as production-ready.

Using a terminal-capable AI Agent? Give it the
[Agent-assisted setup contract](AGENT_QUICKSTART.md). That contract pins the
release and commit, preserves this loopback-only path, and requires a structured
verification report.

## What this path verifies

The primary Quick Start:

- builds the local Docker image from the checked-out source;
- downloads pinned BGE-M3 embedding and bge-reranker-v2-m3 reranker assets;
- waits until the server, models, and source-backed demo fixture are ready;
- publishes only the API at `127.0.0.1:8420`;
- keeps every external LLM role and the built-in Agent disabled;
- sends a plural `namespaces` query with `rerank=true`;
- returns source-backed demo content with numeric reranking scores.

It does not establish PDF, DOCX, image, OCR, authentication, internet-facing deployment, retrieval-quality benchmark, or production-readiness coverage.

## Prerequisites

- Git
- Docker with Docker Compose
- `curl`
- enough local space for the image, build cache, and about 1.17 GB of pinned model assets

## 1. Clone and start

```bash
git clone https://github.com/cortrix/cortrix.git
cd cortrix
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

The first start builds the local image and downloads about 1.17 GB of pinned model assets into the `cortrix-data` volume. It can take several minutes. Later starts reuse that volume.

No `.env` file, LLM provider key, host-side model tooling, manual model download, model conversion, or separate bootstrap command is required.

## 2. Check readiness

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

Compose does not report the service healthy until the API, embedding model, reranker model, and source-backed demo fixture are ready. Model downloads fail closed on missing files, size drift, checksum drift, symlinks, or download errors.

## 3. Run a reranked query

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["demo"],
    "query": "What does semantic storage keep close to the agents that need it?",
    "top_k": 5,
    "rerank": true
  }' \
  http://127.0.0.1:8420/api/v1/query
```

The response should include content from `quickstart-demo.txt` and numeric `rerank_score` values.

## 4. Stop or reset

```bash
docker compose -f deploy/docker-compose.yml down
```

To remove the cached data and model assets as well:

```bash
docker compose -f deploy/docker-compose.yml down --volumes
```

The Quick Start publishes only `127.0.0.1:8420`. It does not publish the metrics or Agent ports. Treat it as a local first-value path, not an internet-facing deployment recipe.

## Publishing on a LAN address (opt-in)

By default nothing outside the host can reach the stack — a loopback-only
publish means teammates cannot open the web UI or API from their own machines,
which is usually the first thing a shared test deployment needs. To publish on
a routable address, set `CORTRIX_PUBLISH_HOST` (and preferably a distinctive
port) when starting the stack:

```bash
CORTRIX_PUBLISH_HOST=10.0.0.5 CORTRIX_HTTP_PORT=18420 \
  docker compose -f deploy/docker-compose.yml up -d
```

The web UI and API are then reachable at `http://10.0.0.5:18420/` from any
machine that can route to that address.

Scope and safety:

- The default stays loopback-only; this is an explicit opt-in, and the
  loopback-only Quick Start contract above is unchanged when the variable is
  unset.
- Everything the API allows becomes available to that network segment. The
  Quick Start profile runs without API-key authentication, so publish beyond
  loopback only on an isolated or trusted test network — or configure
  API-key auth first (see `config.yaml.example`, `auth` section).
- Pick a distinctive high port (for example `18420`) rather than `80`/`8080`
  to avoid colliding with other services on shared test hosts.

## Model provenance and integrity

[`deploy/model-manifest.tsv`](../deploy/model-manifest.tsv) pins the repository, revision, source path, expected size, SHA-256, upstream repository, upstream revision, and upstream license for every downloaded asset.

- Embedding: `onnx-community/bge-m3-ONNX@25b9af8e87a38eb120cfe87125383677b9cd309e`, derived from `BAAI/bge-m3@5617a9f61b028005a4858fdac845db406aefb181`.
- Reranker: `onnx-community/bge-reranker-v2-m3-ONNX@6f5ff65298512715a1e669753bc754d2bc8f367b`, derived from `BAAI/bge-reranker-v2-m3@953dc6f6f85a1b2dbfca4c34a2796e7dde08d41e`.

The bootstrap downloads the exact manifest entries, verifies their size and SHA-256, installs them atomically, and keeps readiness false on any mismatch.

## Local neural models versus LLM roles

`LLM_ENABLED=false` does not mean embeddings or reranking are disabled. This Quick Start uses two local neural ONNX models but makes no request to an external LLM provider.

LLM roles such as `semantic_llm`, `vision_llm`, `agent_llm`, `doc_summary_llm`, and `enricher_llm` are separate optional surfaces. Configure them only when testing the features that consume those roles.

## Query contract

The current query request uses a plural array:

```json
{
  "query": "How should an agent recover from an expired setup token?",
  "namespaces": ["your_namespace"],
  "top_k": 5,
  "rerank": true,
  "include_sources": true
}
```

The deprecated singular `namespace` query field is rejected. Other endpoints may still use a singular namespace field where their OpenAPI schema requires it.

## Deeper source-build verification

The [First-value SupportOps demo](../examples/first-value-supportops/README.md) provides a separate source-build evidence workflow with model identity, execution-provider, inference-counter, trace, cleanup, and server-stop assertions. It is useful for maintainers and release verification, but it is not required for the Docker Quick Start.

That workflow also retains an explicit `onnx-off-contract` profile for low-cost API and fixture testing. It uses stub embedding and `rerank=false`, so it cannot establish semantic embedding or reranker behavior.

## Next steps

- [Install with an AI Agent](AGENT_QUICKSTART.md)
- [Agent access](agent-access.md)
- [OpenAPI spec](../api/openapi.yaml)
- [Compatibility and known status](compatibility.md)
- [Linux NVIDIA CUDA operations](operations/cuda-execution-provider.md)
- [Model provenance and advanced model setup](../deploy/MODELS.md)
