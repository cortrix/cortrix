---
title: Stack fit
description: "How Cortrix relates to your existing stack: keep, add, replace, or unknown."
weight: 20
---

When you bring Cortrix into an existing stack, these decision cards show what you keep, what you add, and what you still need to verify yourself.

The cards aren't replacement guarantees or product rankings. Each card proves only the boundary it states.

## Vocabulary {#vocabulary}

| Term | Meaning |
|---|---|
| `keep` | The existing component or source of truth stays in place |
| `add` | Cortrix is introduced alongside the existing component |
| `replace` | A tested substitution exists for the stated boundary |
| `unknown` | The evidence isn't sufficient to choose yet |
| `not_supported` | The current evidence explicitly excludes the path |

## PostgreSQL with pgcortrix {#postgresql}

> [!NOTE]
> pgcortrix is in development and not supported yet. This card records what the evidence does and doesn't establish. It isn't a recommendation to adopt it now.

| Decision field | What the evidence supports |
|---|---|
| Existing stack | PostgreSQL 13 through 17 with `plpython3u`, plus a separately running Cortrix service |
| Decision | `keep` PostgreSQL as the business-data system. `add` the pgcortrix SQL-to-HTTP bridge and Cortrix. `replace` isn't claimed |
| Tested identity | Cortrix `4a6299ca86c7bec21ed7b8989a729a198fe5a42a`, with the standalone SQL-contract and helper tests under `sql-extensions/pgcortrix/tests/`. The repository marks live PostgreSQL load and `pg_regress` integration as not yet validated |
| Data movement | Explicit SQL functions submit selected content to Cortrix over HTTP. Change data capture, automatic table mirroring, and two-way sync aren't proven |
| Who owns sync | The application or operator owns selection, submission, resubmission, deletion, and consistency checks |
| Query path | PostgreSQL SQL function → `plpython3u` helper → Cortrix HTTP API → rows returned to SQL |
| Failure domain | Extension installation, PL/Python availability, endpoint policy, network transport, Cortrix health, asynchronous indexing, and query execution are separate failure points |
| Security responsibility | A PostgreSQL superuser installs the extension. Operators own the endpoint setting, API key handling, Cortrix authentication, namespace permissions, network policy, and tenant boundaries. Security validation on a live PostgreSQL hasn't been done yet |
| Exit and rollback | Stop calling the SQL functions and remove the extension following PostgreSQL procedures. The source data in PostgreSQL remains authoritative. Namespaces and submitted copies in Cortrix need a separate cleanup decision |
| Evidence | The [pgcortrix README](https://github.com/cortrix/cortrix/blob/release/1.0/sql-extensions/pgcortrix/README.md), [SQL contract tests](https://github.com/cortrix/cortrix/blob/release/1.0/sql-extensions/pgcortrix/tests/test_sql_contract.py), [helper tests](https://github.com/cortrix/cortrix/blob/release/1.0/sql-extensions/pgcortrix/tests/test_helper.py), and [SQL and helper seam tests](https://github.com/cortrix/cortrix/blob/release/1.0/sql-extensions/pgcortrix/tests/test_sql_helper_seam.py) |

**Boundary**: this card supports evaluating the bridge locally as an addition. It doesn't establish compatibility with managed PostgreSQL, live migration, automatic synchronization, production hardening, or a replacement for PostgreSQL.

See [pgcortrix](/docs/integrations/pgcortrix/).

## BGE-M3 embedding plus reranking {#embedding-and-rerank}

| Decision field | What the evidence supports |
|---|---|
| Existing stack | An application with source documents and an existing retrieval path. Compatibility with any named third-party vector database is `unknown` |
| Decision | `keep` the original source documents and the application's ownership of them. `add` Cortrix as a retrieval service. Whether an existing vector database can be `replace`d is `unknown` until a test against that specific stack proves data, query, and rollback parity |
| Data movement | Source documents are ingested into Cortrix and indexed with BGE-M3. The measured profile reranks with bge-reranker-v2-m3. This creates a copy and an index managed by Cortrix |
| Who owns sync | The application or operator owns the initial ingest, propagating updates and deletes, draining tasks, and mapping source-document identity |
| Query path | Application → Cortrix `/api/v1/query` → vector retrieval → reranking → mapping back to source documents. The measured profile sets `rerank=true` and `rag_fusion=false`, with LLM stages off |
| Tested identity | Benchmark bundle commit `4b94390c1d5f7be95065e7483362ec7f93774ed7`, Cortrix `79a4eb17c62521338d1ac47a9749e6230e87e69b`, and public runner `9490520c24a96ed97b80073ed3ebab096b80550b`. Full-corpus SciFact, NFCorpus, and FiQA with every judged test query, plus full-corpus Quora with a deterministic 2,000-query subset |
| Failure domain | Model availability, the execution providers for embedding and reranking, ingest tasks, Cortrix storage and indexing, source mapping, and the calling application are separate failure points |
| Security responsibility | Operators own Cortrix authentication, namespace permissions, model provenance, network exposure, source-data policy, and deletion verification. The benchmark doesn't prove tenant isolation or production security |
| Exit and rollback | Keep the original corpus and the previous query path until acceptance. Stop routing queries to Cortrix and delete the test namespaces explicitly. Exporting to a third-party index and zero-downtime cutover aren't claimed |
| Evidence | The [pinned bundle README](https://github.com/cortrix/cortrix-benchmarks/blob/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1/README.md), its [manifest](https://github.com/cortrix/cortrix-benchmarks/blob/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1/manifest.json), and the per-cell scorecards in the same bundle |

**Boundary**: the measurements this card refers to cover retrieval quality at `top_k=10` only. See [Benchmark evidence](/docs/resources/benchmarks/).

## Next steps {#next-steps}

- [Benchmark evidence](/docs/resources/benchmarks/)
- [Compatibility and status](/docs/resources/compatibility/)
