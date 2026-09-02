# When to rerank, and when not to

Cortrix runs a cross-encoder reranker (bge-reranker-v2-m3) over the candidates a
query retrieves. It is on by default, and on question-answering style workloads it
earns its place. On duplicate-detection and similar-item workloads it does not:
measured on BEIR Quora it costs about **59% of nDCG@10** and **57% of recall@10**
while adding roughly 17 seconds of mean CPU query latency.

This page explains when that happens, why, and how to turn reranking off.

## The two workload shapes

A cross-encoder scores "how relevant is this passage to this query". That is the
right question when the user is looking for material that *answers* or *supports*
their query. It is the wrong question when the user is looking for items that *are
the same thing* as their query.

Measured on a 32-core CPU-only box, full corpora, serial queries:

| Dataset | Task | Dense only | + cross-encoder rerank |
|---|---|---|---|
| SciFact | scientific claim verification | 0.5942 | **0.6184** |
| NFCorpus | nutrition/medical QA | 0.2991 | **0.3121** |
| FiQA | financial forum QA | 0.2540 | **0.3125** |
| Quora | duplicate-question detection | **0.5003** | 0.2031 |

(nDCG@10. The Quora row is 2,000 queries against 522,931 documents; the others are
the full query set against the full corpus.)

The three QA-style datasets gain 0.013–0.059. Quora loses 0.297.

### Why the same model helps and hurts

On Quora the ground truth is a *duplicate*: a different phrasing of the same
question. Relevance and duplication diverge sharply there, and the reranker
optimises relevance.

A worked example, same query against the same 8 namespaces:

> Query: `What is the step by step guide to invest in share market in india?`

- Dense retrieval ranks the verbatim-identical question first — that is the
  ground-truth duplicate.
- With reranking on, that document does not appear anywhere in the 50 returned
  results. The new top four are all topically adjacent but different questions
  ("What are some of the best ways to invest money in India?", "How can I study and
  invest in the Indian share market?", …).

Two mechanisms combine. The cross-encoder scores the topically-broader passages
above the near-identical one, and the executor over-fetches candidates and truncates
only *after* reranking, so a document that was first before reranking can fall out
of the result set entirely rather than merely moving down.

Adding LLM listwise reranking on top recovers part of the loss (0.2031 → 0.3039 on
Quora) but does not reach the dense-only baseline. Reranking is the wrong stage for
this workload, not a stage that needs a better model.

## Turning reranking off

Set `rerank` to `false` on the query:

```json
{
  "query": "What is the step by step guide to invest in share market in india?",
  "namespaces": ["faq-dedup"],
  "top_k": 10,
  "rerank": false
}
```

This is currently the **only** control that takes effect. The request flag is read
in `query_wiring.cpp` and `cross_ns_query_handler.cpp` and gates the reranking stage
directly.

### The namespace-level setting does not work yet

`namespaces.reranker_config` accepts `{"enabled": false}`, and
`RerankerConfigResolver` implements the documented request → namespace → global
priority. Neither is consulted by the live query path: the resolver has no caller
outside its own translation unit, and the executor gates only on the per-request
boolean.

Until that is wired, a namespace whose workload is duplicate detection cannot be
configured once — every caller has to remember `rerank": false` on every request,
and a caller that forgets gets the degradation above with no signal. Tracked in
[#75](https://github.com/cortrix/cortrix/issues/75).

## Choosing for your workload

Rerank when the query is a question and the corpus holds material that answers it:
support tickets searched for solutions, documentation search, claim verification,
retrieval feeding a RAG answer.

Do not rerank when query and corpus are the same kind of short item and the task is
to find matches. This is **measured** for duplicate-question detection (the Quora row
above). For the neighbouring cases — duplicate tickets, near-duplicate product
listings, "find similar" surfaces, deduplication passes over an import — the same
criterion mismatch is a **hypothesis**, not a measurement: they share the shape
(short items, query and corpus of the same kind, ground truth being identity rather
than relevance), but we have not measured them. Treat them as worth testing on your
own data, not as established results.

If you are unsure, measure both on your own data — the direction is a property of
the task, not of the corpus size or language. On the shared-namespace Quora cells,
mean latency increases from 1.7 s without reranking to 19.0 s with the
cross-encoder. Switching it off for a workload that does not benefit is a latency
and cost win as well as a quality one.

## Measurement conditions

These numbers are published in the immutable
[four-corpus CPU measurement bundle](https://github.com/cortrix/cortrix-benchmarks/tree/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1),
measured against Core `79a4eb17c62521338d1ac47a9749e6230e87e69b`.
The public runner is pinned at
`9490520c24a96ed97b80073ed3ebab096b80550b`.

- **Corpora**: BEIR SciFact (5,183 docs / 300 queries), NFCorpus (3,633 / 323),
  FiQA (57,638 / 648), Quora (522,931 docs; the reported row is the first 2,000
  of 10,000 judged queries).
- **Models**: bge-m3 embeddings and bge-reranker-v2-m3, both ONNX fp32 on CPU.
- **Hardware**: 32-core Xeon Silver 4110, 192 GB RAM, no GPU.
- **Query shape**: serial (one query at a time), `top_k=10`, scored as nDCG@10 and
  recall@10 after de-duplicating results by `doc_id`.
- **Comparability**: the Quora arms query the same eight namespaces, so the
  `rerank` flag is the only variable there. The SciFact, NFCorpus, and FiQA arms
  use independently ingested namespaces; their small differences carry the
  bundle's measured variation floor of roughly 0.001–0.003 nDCG.

The bundle measures retrieval quality at `top_k=10`. It is not evidence of answer
quality, concurrent production latency or capacity, security or compliance,
competitive ranking, or business outcomes. Historical p50/p95 values use the
documented measuring-runner percentile convention; the mean latencies quoted on
this page are unaffected.

The direction, not the absolute values, is what this page asks you to act on — and
the direction is what you should verify on your own corpus before choosing.
