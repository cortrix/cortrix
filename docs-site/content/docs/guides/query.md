---
title: Query and rerank
description: Run hybrid retrieval over one or more namespaces, with reranking.
weight: 20
---

On this page you'll query one namespace and then several, learn to read the scores and metadata in a response, and see what to do when part of a query fails.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- A namespace with processed documents. This page reuses the `guide` namespace from [Upload documents](/docs/guides/upload-documents/).

## Run a query {#run-a-query}

Every query goes through one endpoint, `POST /api/v1/query`. `query` and `namespaces` are required:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["guide"],
    "query": "How long do refunds take?",
    "top_k": 3,
    "rerank": true
  }' \
  http://127.0.0.1:8420/api/v1/query
```

The response has two parts, `meta` and `results`. Only the key fields of `results` are shown here:

```json
{
  "meta": {
    "coverage_ratio": 1.0,
    "latency_ms": 222,
    "namespaces_failed": [],
    "namespaces_queried": ["guide"],
    "namespaces_succeeded": ["guide"],
    "warnings": []
  },
  "results": [
    {
      "content": "Refund policy\nCustomers can request a refund within 30 days of purchase. ...",
      "metadata": { "source_path": "refund-policy.txt" },
      "namespace": "guide",
      "rerank_score": 0.824,
      "score": 0.0167
    },
    {
      "content": "Shipping policy. Orders ship within 2 business days. ...",
      "metadata": { "source_path": "shipping-policy.txt" },
      "namespace": "guide",
      "rerank_score": 0.0,
      "score": 0.0164
    }
  ]
}
```

The refund policy, which is relevant to the question, comes first with a rerank score of 0.824. The unrelated shipping policy gets a rerank score of 0.

## Request parameters {#request-parameters}

| Parameter | Type | Default | Description |
|---|---|---|---|
| `query` | string | Required | The query text |
| `namespaces` | string array | Required | The namespaces to search |
| `top_k` | integer | `10` | How many results to return. The minimum is 1 |
| `rerank` | boolean | `true` | Whether to reorder candidates with the reranker model |
| `include_sources` | boolean | — | When `true`, results include source information |
| `filter` | object | — | Filter conditions |
| `timeout_ms` | integer | — | A time budget for the query. The spec defines a range of 100 to 30000 milliseconds. The server's request validation enforces the range, but in testing a value of 50 sent with the `namespaces` array was accepted. Don't rely on the server to enforce it |

For the full request shape, see the [HTTP API reference](/docs/reference/api/).

> [!WARNING]
> Use the plural `namespaces` array. A singular `namespace` is rejected:
>
> ```json
> {
>   "error": {
>     "category": "permanent",
>     "code": "CX_ERR_DEPRECATED_FIELD",
>     "message": "Field 'namespace' is deprecated, use 'namespaces' array instead",
>     "retryable": false,
>     "structured_data": { "deprecated_field": "namespace", "use_instead": "namespaces" }
>   }
> }
> ```

## Read the results {#reading-results}

Each result is a block:

| Field | Meaning |
|---|---|
| `content` | The block's text |
| `namespace` | The namespace the block belongs to |
| `metadata.source_path` | The original filename of the source document |
| `rerank_score` | The relevance score from the reranker model |
| `score` | The score after fusing the retrieval paths |

Use `rerank_score` to judge whether a result is relevant to the question. `score` is the fused score used for ordering. Its values are small and aren't a good absolute measure of relevance.

## Query across namespaces {#cross-namespace}

List several namespaces in `namespaces`, and the results are merged into one ranked list:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["guide", "demo"],
    "query": "How long do refunds take?",
    "top_k": 3
  }' \
  http://127.0.0.1:8420/api/v1/query
```

Each result's `namespace` field tells you where it came from. Use `["*"]` to query every namespace you have access to, up to 100.

## Handle partial failure {#partial-failure}

When you query across namespaces, the request still returns HTTP 200 even if some namespaces fail. Check `meta` to see whether the result is complete:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"namespaces": ["guide", "nope"], "query": "refunds", "top_k": 2}' \
  http://127.0.0.1:8420/api/v1/query
```

Here `nope` doesn't exist, and the response `meta` looks like this. Note that a missing or misspelled namespace is reported as `CX_ERR_INDEX_CORRUPT`, not as a not-found error:

```json
{
  "coverage_ratio": 0.5,
  "namespaces_failed": [
    {
      "category": "permanent",
      "error_code": "CX_ERR_INDEX_CORRUPT",
      "message": "CX_ERR_INDEX_CORRUPT",
      "namespace": "nope",
      "retry_after_ms": null,
      "retryable": false,
      "structured_data": { "index_state": "unavailable", "namespace": "nope" }
    }
  ],
  "namespaces_queried": ["guide", "nope"],
  "namespaces_succeeded": ["guide"]
}
```

> [!IMPORTANT]
> Don't judge completeness from the HTTP status code alone. A `coverage_ratio` below 1 means the results cover only some of the namespaces. Every entry in `namespaces_failed` carries `retryable` and `category`, which you can use to decide whether to retry.

## Turn off reranking {#disable-rerank}

Set `rerank` to `false` to skip reranking:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["guide"],
    "query": "How long do refunds take?",
    "top_k": 3,
    "rerank": false
  }' \
  http://127.0.0.1:8420/api/v1/query
```

In testing, the response still carried a `rerank_score` field with reranking off, but with completely different values. In the example above, the first result went from 0.824 to 0.033. Don't compare scores across the two modes.

For when reranking helps and when it doesn't, see [When to rerank](https://github.com/cortrix/cortrix/blob/release/1.0/docs/operations/reranking-applicability.md).

## See how a query ran {#explain}

Add `?explain=true` and the response gains an `explain` section describing the path the query took:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"namespaces": ["guide"], "query": "How long do refunds take?", "top_k": 1}' \
  'http://127.0.0.1:8420/api/v1/query?explain=true'
```

```json
{
  "complexity_score": 0.8999999761581421,
  "granularity": "auto",
  "query": "How long do refunds take?",
  "routing_path": "simple",
  "rrf_path_counts": { "dense": 1, "fts5": 1 },
  "via_path_counts": { "chunk": 1 }
}
```

The output is shortened. The full object has more fields, including `ns_id`, `routing_decision_source`, `crag_verdict`, `crag_score`, and `llm_dependent_features`.

`rrf_path_counts` shows that this query used both vector search (`dense`) and full-text search (`fts5`), and that the two result sets were then fused and ranked.

## Next steps {#next-steps}

- [Work with memory](/docs/guides/memory/)
- [Python SDK](/docs/integrations/python-sdk/): run the same query with `client.search()`.
- [HTTP API reference](/docs/reference/api/)
