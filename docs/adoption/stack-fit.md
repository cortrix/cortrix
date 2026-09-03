# Stack fit and adoption boundaries

These decision cards describe what a team keeps, adds, or still needs to verify. They are not replacement guarantees or competitor rankings. A source file, adapter, or test proves only the boundary stated in its card.

Status vocabulary:

- `keep`: the existing component or source of truth remains in place.
- `add`: Cortrix is introduced alongside the existing component.
- `replace`: a tested substitution exists for the stated boundary.
- `unknown`: evidence is not sufficient to choose yet.
- `not_supported`: the current evidence explicitly excludes the path.

## PostgreSQL with pgcortrix

| Decision field | Evidence-backed value |
| --- | --- |
| Existing stack | PostgreSQL 13–17 with `plpython3u`, plus an independently running Cortrix server. |
| Fit decision | `keep` PostgreSQL as the business-data system; `add` the pgcortrix SQL-to-HTTP bridge and Cortrix. `replace` is not claimed. |
| Data movement | Explicit SQL functions submit selected content to Cortrix over HTTP. No database CDC, automatic table mirroring, or bidirectional sync is proven. |
| Sync owner | The application or operator owns selection, submission, re-submission, deletion, and consistency checks. |
| Query path | PostgreSQL SQL function → `plpython3u` helper → Cortrix HTTP API → result rows returned to SQL. |
| Failure domain | PostgreSQL extension installation, PL/Python availability, endpoint policy, network transport, Cortrix health, asynchronous indexing, and query execution remain separate failure points. |
| Security responsibility | A PostgreSQL superuser installs the extension. Operators own `pgcortrix.endpoint`, API-key handling, Cortrix auth, namespace permissions, network policy, and tenant boundaries. Offline helper tests cover endpoint allow/block behavior; a registered `PGC_SUSET` custom GUC and live PostgreSQL security validation are not yet proven. |
| Tested identity | Cortrix `4a6299ca86c7bec21ed7b8989a729a198fe5a42a`; standalone SQL-contract/helper tests under `sql-extensions/pgcortrix/tests/`. The repository explicitly marks live PostgreSQL load and `pg_regress` integration as not yet validated in this environment. |
| Exit / rollback | Stop invoking the SQL functions and remove the extension according to PostgreSQL procedures. PostgreSQL source data remains authoritative. Cortrix namespaces and submitted copies require a separate explicit cleanup decision. |
| Evidence | [pgcortrix README](../../sql-extensions/pgcortrix/README.md), [SQL contract tests](../../sql-extensions/pgcortrix/tests/test_sql_contract.py), [helper tests](../../sql-extensions/pgcortrix/tests/test_helper.py), and [SQL/helper seam tests](../../sql-extensions/pgcortrix/tests/test_sql_helper_seam.py). |

Adoption boundary: this card supports an additive local bridge evaluation. It does not establish managed PostgreSQL compatibility, live migration, automatic synchronization, production hardening, or a replacement for PostgreSQL.

## BGE-M3 embedding plus reranking

| Decision field | Evidence-backed value |
| --- | --- |
| Existing stack | An application with source documents and an existing retrieval path. Named third-party vector database compatibility is `unknown`. |
| Fit decision | `keep` original source documents and application ownership; `add` Cortrix as a retrieval service. Whether an existing vector database can be `replace`d is `unknown` until a named-stack test proves data, query, and rollback parity. |
| Data movement | Source documents are ingested into Cortrix and indexed with BAAI/bge-m3; the measured profile reranks with BAAI/bge-reranker-v2-m3. This creates a Cortrix-managed copy/index. |
| Sync owner | The application or operator owns initial ingest, update/delete propagation, task drain, and source-document identity mapping. |
| Query path | Application → Cortrix `/api/v1/query` → embedding retrieval → reranker → source-document mapping. The measured profile sets `rerank=true`, `rag_fusion=false`, and disables LLM stages. |
| Failure domain | Model availability, embedding/reranker execution provider, ingest tasks, Cortrix storage/indexing, source mapping, and the calling application remain separate failure points. |
| Security responsibility | Operators own Cortrix auth, namespace permissions, model provenance, network exposure, source-data policy, and deletion verification. The benchmark does not prove tenant isolation or production security. |
| Tested identity | Canonical benchmark bundle commit `4b94390c1d5f7be95065e7483362ec7f93774ed7`; Core `79a4eb17c62521338d1ac47a9749e6230e87e69b`; public runner `9490520c24a96ed97b80073ed3ebab096b80550b`; full-corpus SciFact, NFCorpus, and FiQA with every judged test query, plus full-corpus Quora with a deterministic 2,000-query subset. |
| Exit / rollback | Retain the original source corpus and the previous query path until acceptance. Stop routing queries to Cortrix and explicitly delete test namespaces. No third-party index export or zero-downtime cutover is claimed. |
| Evidence | [Pinned bundle README](https://github.com/cortrix/cortrix-benchmarks/blob/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1/README.md), [manifest](https://github.com/cortrix/cortrix-benchmarks/blob/4b94390c1d5f7be95065e7483362ec7f93774ed7/results/published/beir-four-corpus-cpu-2026-08-v1/manifest.json), and per-cell scorecards in the same immutable bundle. |

Measurement boundary: the referenced bundle measures retrieval quality at `top_k=10` only. It does not establish end-to-end answer quality, universal domain performance, concurrent production latency or capacity, security properties, business outcomes, or compatibility with a named external vector database. The SciFact, NFCorpus, and FiQA arm comparisons include independently ingested namespaces; the Quora arms share the same eight namespaces and are the bundle's only strictly controlled comparison.
