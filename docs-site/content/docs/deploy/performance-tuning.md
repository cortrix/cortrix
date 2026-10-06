---
title: Performance tuning
description: The settings that affect query latency, recall, and ingestion speed, and how to measure them on your own data.
weight: 35
---

A handful of settings affect Cortrix's performance. Learn which stage each one acts on and how to measure its effect on your own data.

> [!IMPORTANT]
> Which way to turn a setting depends on your task and your data. The numbers on this page only illustrate direction. Measure on your own data before you change a default.

## Measure first {#measure-first}

Every query response includes the latency the server measured:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"namespaces": ["demo"], "query": "semantic storage", "top_k": 5}' \
  http://127.0.0.1:8420/api/v1/query
```

```json
{
  "meta": {
    "coverage_ratio": 1.0,
    "latency_ms": 242,
    "namespaces_succeeded": ["demo"]
  }
}
```

Change one variable at a time, repeat the same set of queries several times, and compare the median `meta.latency_ms`. Check result quality too: faster isn't the same as better.

Add `?explain=true` to see which retrieval paths a query used. See [Query and rerank](/docs/guides/query/#explain).

## Settings at a glance {#overview}

| Setting | Where | Default | Affects |
|---|---|---|---|
| `rerank` | Query request | `true` | Query latency and result ordering |
| `top_k` | Query request | `10` | How many results come back, and the candidate pool size |
| `timeout_ms` | Query request | — | The time limit for one query |
| `retrieval.candidate_multiplier` | Configuration file | `3` | Candidate pool size |
| `retrieval.max_candidates` | Configuration file | `50` | The candidate pool ceiling, and so the recall ceiling |
| `embedding.execution_provider` | Configuration file | `auto` | Where embedding runs |
| `reranker.execution_provider` | Configuration file | `auto` | Where reranking runs |
| `server.thread_count` | Configuration file | `4` | The server's worker threads |
| `spc.worker_count` | Configuration file | `2` | How many documents are processed in parallel |
| `spc.embedding_batch_size` | Configuration file | `1` | Blocks embedded per batch |
| `spc.onnx_intra_threads` | Configuration file | `4` | Intra-op threads for ONNX inference |
| `spc.onnx_inter_threads` | Configuration file | `1` | Parallel ONNX inferences |
| `namespace.max_active` | Configuration file | `10` | How many namespaces stay loaded at once |
| `namespace.idle_timeout_s` | Configuration file | `300` | How long a namespace sits idle before it's unloaded |

The configuration file defaults come from `config.yaml.example` in the repository.

## Queries {#query}

### Reranking {#rerank}

Reranking is the single switch with the biggest effect on query latency. When it's on, Cortrix scores each retrieved candidate with the cross-encoder model bge-reranker-v2-m3.

As a rough illustration, here is one measurement taken while writing this page. It isn't a published benchmark, and it isn't in the repository. On the quickstart service, with 3 short documents and CPU inference in Docker on an Apple silicon Mac, the median over 8 queries per setting was:

| Setting | Median `latency_ms` |
|---|---:|
| `rerank: true` | 242 |
| `rerank: false` | 52 |

The measurements published in the repository show that on the Quora corpus (522,931 documents, a 32-core CPU, no GPU, serial queries), mean query latency goes from 1.7 seconds without reranking to 19.0 seconds with it.

Whether reranking is worth that cost depends on the kind of task:

| Kind of task | Examples | Recommendation |
|---|---|---|
| Questions and answers | Documentation search, finding solutions in support tickets, grounding a RAG answer | Leave it on |
| Finding identical or duplicate items | Duplicate-question detection | Turn it off |

In the published measurements, the three question-answering datasets (SciFact, NFCorpus, FiQA) gained 0.013 to 0.059 nDCG@10 with reranking on, while duplicate-question detection (Quora) dropped from 0.5003 to 0.2031. The reranker optimizes for "relevant," and that kind of task is looking for "the same."

For neighboring cases such as duplicate tickets, similar products, or "find similar" features, the same problem is a hypothesis, not a measurement. Test it on your own data.

To turn reranking off, set `"rerank": false` on each query.

> [!WARNING]
> Only the `rerank` flag on the query request takes effect today. The namespace-level `reranker_config` is accepted, but the query path doesn't read it yet. See [cortrix/cortrix#75](https://github.com/cortrix/cortrix/issues/75). For a workload that shouldn't be reranked, every request needs to pass `"rerank": false` explicitly.

For the measurement conditions and full data, see [When to rerank](https://github.com/cortrix/cortrix/blob/release/1.0/docs/operations/reranking-applicability.md) and [Benchmark evidence](/docs/resources/benchmarks/).

### Candidate pool size {#candidate-pool}

Cortrix fetches more candidates than `top_k`, then ranks and truncates. The number of candidates is:

```text
candidate_k = min(top_k × candidate_multiplier × oversample, max_candidates)
```

- A larger `top_k` means more candidates and more content returned.
- `max_candidates` is a hard ceiling. On a large corpus, relevant documents can rank below 50, so raising `max_candidates` is the only way to lift the recall ceiling.

```yaml {title="config.yaml"}
retrieval:
  candidate_multiplier: 3
  max_candidates: 50
```

You can also override these with the `CORTRIX_RETRIEVAL_CANDIDATE_MULTIPLIER` and `CORTRIX_RETRIEVAL_MAX_CANDIDATES` environment variables.

### Query scope {#query-scope}

List only the namespaces you actually need in `namespaces`. `["*"]` queries every namespace you have access to, up to 100.

### Time budget {#timeout}

`timeout_ms` caps how long one query can take. The spec defines a range of 100 to 30000 milliseconds, and the budget is shared across the retrieval paths that run in parallel. It keeps an occasional query from running too long. It doesn't make queries faster.

## Execution providers {#execution-provider}

The embedding model and the reranker each have an `execution_provider` setting. The values are `auto`, `cpu`, `coreml`, and `cuda`:

```yaml {title="config.yaml"}
embedding:
  execution_provider: "auto"

reranker:
  execution_provider: "auto"
```

The matching environment variables are `CORTRIX_EMBEDDING_EXECUTION_PROVIDER` and `CORTRIX_RERANKER_EXECUTION_PROVIDER`. The quickstart pins both to `cpu`.

The readiness check shows which execution provider is actually in use:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

```json
{
  "embedding_execution_provider": {
    "active_ep": "cpu",
    "configured_ep": "cpu",
    "fallback": false,
    "status": "ok"
  }
}
```

`active_ep` is the provider in use. A `fallback` of `true` means the configured provider wasn't available and Cortrix fell back.

Using an NVIDIA GPU on Linux needs a separate image. See [NVIDIA CUDA](/docs/deploy/cuda/). The CUDA document in the repository is a deployment guide and doesn't include performance data.

## Ingestion {#ingestion}

After upload, a document is parsed, chunked, and embedded. These settings control how much of that pipeline runs in parallel:

```yaml {title="config.yaml"}
spc:
  worker_count: 2
  chunk_size: 512
  chunk_overlap: 50
  embedding_batch_size: 1
  onnx_intra_threads: 4
  onnx_inter_threads: 1
```

| Setting | Purpose |
|---|---|
| `worker_count` | Threads processing documents in parallel. 2 to 4 is recommended |
| `embedding_batch_size` | Blocks sent to the embedding model per batch |
| `onnx_intra_threads` | Threads used inside a single ONNX inference |
| `onnx_inter_threads` | ONNX inferences that run in parallel |
| `chunk_size` | The target number of tokens per text block. Smaller blocks mean more blocks per document |

These are configuration-file keys. They have no environment-variable override.

The readiness check reports the current queue depth and worker count under `spc_pipeline`:

```json
{ "spc_pipeline": { "queue_depth": 0, "status": "ok", "workers": 2 } }
```

## Server {#server}

```yaml {title="config.yaml"}
server:
  thread_count: 4

namespace:
  max_active: 10
  idle_timeout_s: 300
```

- `server.thread_count` is the number of worker threads handling requests. The recommended value is your CPU core count. You can override it with `CORTRIX_SERVER_THREADS`.
- `namespace.max_active` is how many namespaces stay loaded at once.
- `namespace.idle_timeout_s` is how long a namespace sits idle before it's unloaded automatically. Set it to `0` to never unload.

## What these numbers do and don't tell you {#boundaries}

The measurements published in the repository cover retrieval quality at `top_k=10` and mean latency under serial queries. They say nothing about production latency or capacity under concurrency, or about end-to-end answer quality.

This page doesn't recommend values for thread counts, batch size, or execution providers, because the repository has no published controlled measurements for them. Measure those on your own hardware and data, using the method above, before you settle on values.

## Next steps {#next-steps}

- [Query and rerank](/docs/guides/query/)
- [NVIDIA CUDA](/docs/deploy/cuda/)
- [Benchmark evidence](/docs/resources/benchmarks/)
- [Configuration](/docs/deploy/configuration/)
