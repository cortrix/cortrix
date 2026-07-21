# Cortrix Docker Quick Start

From a fresh checkout to a source-backed reranked query with Docker. For model
identity and integrity details, see [`MODELS.md`](./MODELS.md).

**Prerequisites:** Git, Docker with Docker Compose, `curl`, and enough local
space for the image, build cache, and about 1.17 GB of pinned model assets.

---

## 1. Clone and start

```bash
git clone https://github.com/cortrix/cortrix.git
cd cortrix
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

The first start downloads about 1.17 GB of pinned model assets and can take
several minutes. Later starts reuse the `cortrix-data` volume. No `.env` file,
LLM key, host-side model tools, manual model download, model conversion, or
separate bootstrap command is required.

## 2. Check readiness

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

Compose waits until the API, embedding model, reranker model, and source-backed
demo fixture are ready. Missing files, size or checksum drift, symlinks, and
download failures keep the service unready.

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

The response should include content from `quickstart-demo.txt` and numeric
`rerank_score` values.

## 4. Stop or reset

```bash
docker compose -f deploy/docker-compose.yml down
```

Add `--volumes` to remove the cached data and model assets as well.

## Scope

The Quick Start uses real BGE-M3 embedding and bge-reranker-v2-m3 reranking on
CPU. External LLM roles and the built-in Agent remain disabled. The API at
`127.0.0.1:8420` is the only published host socket; the metrics and Agent ports
are not exposed. Core's unauthenticated non-loopback guard remains fail closed
by default; this Compose file explicitly permits only the container-internal
wildcard listener needed for that loopback publication.

This path is for local first value. It does not establish parser coverage,
authentication, internet-facing deployment, benchmark quality, or production
readiness. See the canonical [Quick Start](../docs/QUICKSTART.md) for provenance,
integrity checks, and next steps. Maintainers who need the deeper source-build
evidence workflow can use the
[First-value SupportOps demo](../examples/first-value-supportops/README.md).
