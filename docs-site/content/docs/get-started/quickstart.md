---
title: Quickstart
description: Start Cortrix locally with Docker and run your first query.
weight: 20
---

In this quickstart you'll start Cortrix `v1.0.0-rc.2` on your machine with Docker and run a reranked query against the bundled demo document. The whole path uses real local embedding and reranking models, and you don't need a key for any LLM service.

If you'd rather have an AI agent run these steps, see [Install with an AI agent](/docs/get-started/agent-quickstart/).

## Prerequisites {#prerequisites}

- Git
- Docker with Docker Compose
- `curl`
- Enough disk space for the image, the build cache, and about 1.17 GB of model files

You don't need a `.env` file, an LLM provider key, model tooling on the host, or any manual model download or conversion.

## Steps {#steps}

{{% steps %}}

### Clone and start {#clone-and-start}

```bash
git clone --branch v1.0.0-rc.2 --depth 1 \
  https://github.com/cortrix/cortrix.git
cd cortrix
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

The first start builds the image from source and downloads the model files into the `cortrix-data` volume (its full name in Docker is `deploy_cortrix-data`). Later starts reuse that volume.

The command returns only when the service is ready. It ends with:

```text
 Container deploy-cortrix-1 Started
 Container deploy-cortrix-1 Waiting
 Container deploy-cortrix-1 Healthy
```

> [!NOTE]
> How long the first start takes depends on your machine and network. On an Apple silicon Mac with 5 CPUs assigned to Docker, the image build took about 6 minutes and the model download took under a minute.

> [!TIP]
> If you see `docker: unknown command: docker compose`, the Compose plugin isn't installed. If you have the standalone version, replace `docker compose` with `docker-compose` in each command.

### Check readiness {#check-readiness}

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

`status` should be `ready`, and every component's `status` should be `ok`:

```json
{
  "components": {
    "catalog": { "catalog_db_open": true, "status": "ok" },
    "disk": { "stage": "warn", "status": "ok" },
    "embedding_execution_provider": {
      "active_ep": "cpu",
      "configured_ep": "cpu",
      "fallback": false,
      "model_configured": true,
      "policy_mismatch": false,
      "preferred_ep": "cpu",
      "status": "ok"
    },
    "quickstart_demo": { "proof_file_present": true, "status": "ok" },
    "reranker_execution_provider": {
      "active_ep": "cpu",
      "configured_ep": "cpu",
      "fallback": false,
      "model_configured": true,
      "policy_mismatch": false,
      "preferred_ep": "cpu",
      "status": "ok"
    },
    "secret_provider": { "provider_type": "env", "status": "ok" },
    "spc_pipeline": { "queue_depth": 0, "status": "ok", "workers": 2 }
  },
  "status": "ready",
  "version": "1.0.0-rc.2"
}
```

Compose reports the service healthy only once the API, the embedding model, the reranker model, and the demo document are all ready. If a model download hits a missing file, a size or checksum mismatch, a symbolic link, or a download error, it fails outright rather than starting with incomplete models.

### Run a reranked query {#run-a-query}

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

The response should include content from `quickstart-demo.txt` and a numeric `rerank_score`. Here is a real response, with `metadata` shortened:

```json
{
  "meta": {
    "coverage_ratio": 1.0,
    "crag_verdict": "incorrect",
    "deduplicated_chunks": [],
    "deduplicated_chunks_count": 0,
    "latency_ms": 367,
    "namespaces_failed": [],
    "namespaces_queried": ["demo"],
    "namespaces_succeeded": ["demo"],
    "warnings": []
  },
  "results": [
    {
      "child_id": "01M409ADX4M34WZD0EWVRYEMQJ",
      "content": "Cortrix QuickStart demo\nCortrix provides semantic storage for agentic applications. In this demo,\nsemantic storage keeps durable context close to the agents that need it.\nThe local embedding model finds relevant passages, and the local reranker\norders those passages without sending the document or query to an LLM service.",
      "metadata": {
        "source_path": "quickstart-demo.txt",
        "mime_type": "text/plain"
      },
      "namespace": "demo",
      "rerank_score": 0.9929662942886353,
      "score": 0.01666666753590107
    }
  ]
}
```

Identifiers, latency, and scores will differ on your machine.

{{% /steps %}}

## Stop or reset {#stop-or-reset}

To stop the service and keep your data and the downloaded models:

```bash
docker compose -f deploy/docker-compose.yml down
```

To remove the data and models as well:

```bash
docker compose -f deploy/docker-compose.yml down --volumes
```

> [!WARNING]
> `--volumes` deletes the `cortrix-data` volume. The next start downloads about 1.17 GB of models again.

## What this path verifies {#what-this-verifies}

The quickstart:

- Builds the local Docker image from the checked-out source.
- Downloads pinned versions of the BGE-M3 embedding model and the bge-reranker-v2-m3 reranker.
- Publishes only the API, at `127.0.0.1:8420`. The metrics port and the built-in Agent port aren't published.
- Keeps every external LLM role and the built-in Agent turned off.
- Sends a query with a plural `namespaces` array and `rerank=true`, and gets back source-backed demo content with numeric rerank scores.

It does **not** cover PDF, DOCX, images, OCR, authentication, internet-facing deployment, or retrieval-quality benchmarks, and it doesn't establish production readiness.

> [!IMPORTANT]
> The quickstart service has no authentication. By default Compose publishes it only on the host's loopback address, so other machines can't reach it. Don't treat it as an internet-facing deployment.

## Model provenance and integrity {#model-provenance}

`deploy/model-manifest.tsv` in the repository pins the repository, revision, path, expected size, SHA-256, upstream repository, upstream revision, and upstream license for every downloaded file.

| Purpose | Model | Source |
|---|---|---|
| Embedding | `onnx-community/bge-m3-ONNX` | Derived from `BAAI/bge-m3` |
| Reranking | `onnx-community/bge-reranker-v2-m3-ONNX` | Derived from `BAAI/bge-reranker-v2-m3` |

At startup, Cortrix downloads each manifest entry, verifies its size and SHA-256, and installs it atomically. If anything doesn't match, the service doesn't report ready.

## Next steps {#next-steps}

- [Core concepts](/docs/get-started/core-concepts/): understand the namespace, blocks, and query you just used.
- [Choose an access path](/docs/integrations/overview/): connect to the running service through MCP or the Python SDK.
- [Compatibility and status](/docs/resources/compatibility/): see the current status of every capability.
