---
title: Benchmark evidence
description: The retrieval-quality measurements that have been published, and what they apply to.
weight: 30
---

Cortrix has published one set of retrieval-quality measurements. Here are the conditions they were taken under, and what the numbers do and don't tell you.

## Measurement bundle {#bundle}

The measurements accepted for public use are the pinned [four-corpus CPU measurement bundle](https://github.com/cortrix/cortrix-benchmarks/tree/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1), which holds results for 16 retrieval configurations.

| Item | Identity |
|---|---|
| The Cortrix commit measured | [`79a4eb17c62521338d1ac47a9749e6230e87e69b`](https://github.com/cortrix/cortrix/commit/79a4eb17c62521338d1ac47a9749e6230e87e69b) |
| The public runner | [`9490520c24a96ed97b80073ed3ebab096b80550b`](https://github.com/cortrix/cortrix-benchmarks/commit/9490520c24a96ed97b80073ed3ebab096b80550b) |
| The bundle commit | [`4b94390c1d5f7be95065e7483362ec7f93774ed7`](https://github.com/cortrix/cortrix-benchmarks/commit/4b94390c1d5f7be95065e7483362ec7f93774ed7) |

## Measurement conditions {#conditions}

| Item | Details |
|---|---|
| Hardware | A 32-core Xeon Silver 4110 with 192 GB of RAM and no GPU |
| Models | bge-m3 embedding and bge-reranker-v2-m3 reranking, both ONNX fp32 on CPU |
| Query shape | Serial, one query at a time, `top_k=10` |
| Scoring | nDCG@10 and Recall@10 after de-duplicating by `doc_id` |

| Corpus | Documents | Queries |
|---|---:|---:|
| SciFact | 5,183 | 300 |
| NFCorpus | 3,633 | 323 |
| FiQA | 57,638 | 648 |
| Quora | 522,931 | The first 2,000 of 10,000 judged queries |

SciFact, NFCorpus, and FiQA use every judged test query.

## Results {#results}

The table shows nDCG@10:

| Dataset | Task | Dense only | With cross-encoder reranking |
|---|---|---:|---:|
| SciFact | Scientific claim verification | 0.5942 | **0.6184** |
| NFCorpus | Nutrition and medical QA | 0.2991 | **0.3121** |
| FiQA | Financial forum QA | 0.2540 | **0.3125** |
| Quora | Duplicate-question detection | **0.5003** | 0.2031 |

The three question-answering datasets gain 0.013 to 0.059 with reranking on. Quora loses 0.297.

Reranking helps question-answering tasks and hurts duplicate-detection tasks. For why, and what to do about it, see [Performance tuning](/docs/deploy/performance-tuning/#rerank).

## Comparability {#comparability}

- The two Quora configurations query the same 8 namespaces, so the `rerank` flag is the only variable. This is the bundle's only strictly controlled comparison.
- The SciFact, NFCorpus, and FiQA configurations use independently ingested namespaces. Small differences between them fall within the variation floor the bundle discloses, roughly 0.001 to 0.003 nDCG.

## What these numbers don't show {#what-it-does-not-show}

The bundle measures **retrieval quality** at `top_k=10`. It isn't evidence of:

- End-to-end answer quality
- Production latency or capacity under concurrency
- Security or compliance properties
- How Cortrix ranks against other products
- Business outcomes

The latency figures quoted in this documentation are means under serial queries and don't represent behavior under concurrent load.

> [!IMPORTANT]
> Act on the direction these measurements show, not on the absolute values. Verify that direction on your own corpus before you choose.

## Next steps {#next-steps}

- [Performance tuning](/docs/deploy/performance-tuning/)
- [Stack fit](/docs/resources/stack-fit/)
- [Compatibility and status](/docs/resources/compatibility/)
