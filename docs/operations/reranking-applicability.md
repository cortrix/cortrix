# When to rerank, and when not to

Cortrix runs a cross-encoder reranker (bge-reranker-v2-m3) over the candidates a
query retrieves. It is on by default, and on question-answering style workloads it
earns its place. On duplicate-detection and similar-item workloads it does not:
measured on BEIR Quora it costs **62% of nDCG@10** and **59% of recall@10** while
adding roughly 15 seconds of CPU per query.

This page explains when that happens, why, and how to turn reranking off.

## The two workload shapes

A cross-encoder scores "how relevant is this passage to this query". That is the
right question when the user is looking for material that *answers* or *supports*
their query. It is the wrong question when the user is looking for items that *are
the same thing* as their query.

Measured on a 32-core CPU-only box, full corpora, serial queries:

| Dataset | Task | Dense only | + cross-encoder rerank |
|---|---|---|---|
| SciFact | scientific claim verification | 0.5949 | **0.6200** |
| NFCorpus | nutrition/medical QA | 0.3037 | **0.3160** |
| FiQA | financial forum QA | 0.2623 | **0.3221** |
| Quora | duplicate-question detection | **0.4923** | 0.1862 |

(nDCG@10. The Quora row is 2,000 queries against 522,927 documents; the others are
the full query set against the full corpus.)

The three QA-style datasets gain 0.02–0.06. Quora loses 0.306.

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

Adding LLM listwise reranking on top recovers part of the loss (0.1862 → 0.2763 on
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
to find matches: duplicate-question or duplicate-ticket detection, near-duplicate
product listings, "find similar" surfaces, deduplication passes over an import.

If you are unsure, measure both on your own data — the direction is a property of
the task, not of the corpus size or language. The reranker also dominates query
latency on CPU (9–17 s of a typical 10–20 s query in the measurements above), so
switching it off for a workload that does not benefit is a latency and cost win as
well as a quality one.

## Reproducing these numbers

The comparison is reproducible with the public BEIR runner; see
`benchmarks/beir-retrieval-quality/` in the `cortrix-benchmarks` repository for the
profiles and the exact invocations.
