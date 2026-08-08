# Feature Index

Cortrix is developed against an internal design-tracking system. Every
capability gets a tracking number during design — `F13`, `MEM02`, `P08` — and
those numbers appear throughout the codebase: in comments, test names, error
codes, metric names, config keys and file names. They are not noise and they
are not dead references; they are the project's working vocabulary. This
document is the public index for that vocabulary, so the code can be read
as-is.

The index is versioned. Each release line gets its own feature list; a new
release appends a new section and earlier sections are kept unchanged, so an
identifier can always be resolved against the release it was written for.
Within a release section, the tables list only capabilities shipped and
verifiable in this repository at that release; identifiers that appear in the
tree but name unshipped work are listed separately at the end of the section,
so nothing unreleased is presented as part of the shipped surface.

---

## Notation

| Notation | Meaning |
|---|---|
| `F01`–`F48` | Engine-side features: storage, ingest, retrieval, serving. |
| `MEM01`–`MEM05` | Memory-system features. |
| `P01`–`P14` | Product-surface features: SDK, API docs, auth, MCP, web UI. |
| `M-###` / `m-###` | Small fixes carried over from the pre-1.0 codebase. |
| `TD-*` | Deferred-work labels (e.g. `TD-F42-BULK-SUBMIT`): work identified during design and scheduled for a later release. |
| `D0`–`D6` (phase context) | Phases of the internal development cycle: D0 scope, D1 detailed design, D2 planning, D3 implementation, D3.5 integration, D4 testing, D5 acceptance, D6 delivery. A comment like “D3 wiring” or “D3.5 deferred” dates the code to the phase that produced it. The phase series ends at D6. |
| `D1`, `D2`, … (decision context) | Numbered decisions inside one feature’s detailed-design discussion — e.g. `MEM02 D8` (the preference-immunity threshold), the chunker’s `D9 lock` (per-parent granularity), the import layer’s `D7` tenant guard. Numbering restarts for every feature, so a decision number resolves against the nearest feature context in the surrounding code. Values above `D6` (`D7`–`D12` in this release) are always decision numbers, never phases; for `D6` and below, phase references name process activities (scope, wiring, testing) while decision references sit next to a concrete design choice. |
| `Wave A`–`Wave E` | Design and implementation batches within a phase. |
| `GEN-*` | Cross-cutting principles applied to every feature (see below). |
| `§` (e.g. `ARCH § 4.1`) | A section reference into an internal design document. The code statement it accompanies stands on its own; the reference records where the decision was made. |
| `CX_ERR_F##_*` | Wire error codes carry the number of the feature that owns them. |

### Cross-cutting principles (`GEN-*`)

- **GEN-Agent** — every API surface is designed for machine (agent) consumption
  first: errors are structured (`code`, `retryable`, `category`,
  `retry_after_ms`, `structured_data`), states are enumerable, and degraded
  paths still return data instead of failing whole.
- **GEN-OperationLog** — user-visible operations are recorded uniformly through
  the operation log (F18a) rather than ad-hoc logging.
- **GEN-OpenCore** — this repository is self-contained: no conditional
  compilation against code that is not in this tree.

---

## v1.0

The feature set of the `v1.0.x` release line (first published as
`v1.0.0-rc.1`). Every row below names a capability shipped in this repository
in that line; identifiers referenced by the code but not shipped are in the
final subsection.

### Storage and ingest engine

| # | Name | What it is |
|---|---|---|
| F01 | P-HNSW | Persistent HNSW vector index (based on hnswlib) with snapshot + write-ahead-log durability and group commit. |
| F05 | Namespace resource pool | Pooled per-namespace index and database handles: eager startup load, admission control. |
| F06 | Document parser | Pluggable parsing pipeline; Docling by default, page-level fault tolerance, OCR fallback for empty extractions. |
| F07 | Semantic score | Per-block quality score assigned at write time and raised by enrichment. |
| F08 | Metadata block | Per-document metadata block, embedded and indexed so documents are discoverable by their own description. |
| F09 | Block header | Self-describing binary block header with CRC over all fields. |
| F10 | Data cleaning | Exact (BM25) and semantic dedup plus anomaly detection inside the ingest pipeline. |
| F12 | Namespace→unit mapping | Two-layer mapping that introduces the storage-unit abstraction and routing database; also carries per-namespace config overrides. |
| F16a | DB manual import | Import rows from an external database table (per-row and merge modes, JSON-DSL filters, parameterized queries). |
| F21 | Watcher fan-out | A single filesystem watcher fanned out to all namespaces. |
| F25 | Write coordinator | Pending-write log giving atomic writes across the vector index, SQLite and blob storage. |
| F42 | Async document processing | Background task queue with persisted tasks, progress and cancel APIs. |

### Retrieval pipeline

| # | Name | What it is |
|---|---|---|
| F02 | Reranker | Cross-encoder reranking stage (bge-reranker-v2-m3 over ONNX Runtime) with a resident worker pool and circuit breaker. |
| F03 | SPC enricher | LLM enrichment of ingested content — entity extraction and summaries — through a pluggable enricher chain with budget caps and fallbacks. |
| F04 | Cross-namespace query | Scatter-gather querying across namespaces with merged reranking and de-duplication. |
| F34 | Parent-child chunking | Two-level chunking: retrieval matches small child chunks, responses expand to the parent for LLM context. |
| F35 | Contextual retrieval | LLM-generated context prepended to chunks before embedding; contextualized and plain embeddings coexist and fuse at query time. |
| F36 | RAG-Fusion | LLM-generated query variants merged with reciprocal-rank fusion. |
| F37 | CRAG evaluation | A small classifier judges retrieval quality per query; ambiguous results are filtered, failures degrade transparently. |
| F38 | HyPE index | Hypothetical questions pre-generated at index time and indexed alongside the chunks they describe. |
| F39 | Query complexity router | Routes each query — simple, complex, or chat — to a lighter or fuller pipeline. |
| F40 | BGE-M3 sparse retrieval | Sparse vectors from the same BGE-M3 forward pass, served by a SPLADE-style inverted index and fused with dense and FTS results. |
| F41 | Document summary index | Asynchronously generated document summaries indexed as searchable blocks, with text-match fallback when the LLM is unavailable. |

### Memory system

| # | Name | What it is |
|---|---|---|
| MEM01 | Memory scoring | Type-aware decay: facts and preferences are immune, events decay. |
| MEM02 | LLM memory extraction | Facts extracted from conversations with contradiction detection, audit trail and revocation. |
| MEM03 | Memory transparency | List, create, edit and invalidate endpoints over a user's stored memory. |
| MEM04 | Memory immunity | Per-session opt-out from memory extraction. |
| MEM05 | Per-user isolation | `user_id` enforced on every memory search. |

### Serving, operations and quality

| # | Name | What it is |
|---|---|---|
| F13 | Agent observability | Agent trace capture (trace/session identity) with query endpoints over traces and interactions. |
| F14 | pgcortrix | PostgreSQL extension (plpython3u) exposing search, upload and memory as SQL functions (`pgcortrix_*`). |
| F18a | Operation log | Uniform user-visible operation log with a query endpoint and retention cleanup. |
| F20 | Security hardening | Non-root container, secret handling, safe startup defaults, log redaction. |
| F22 | ONNX Runtime upgrades | Runtime version detection, opset validation and an API compatibility layer. |
| F23 | Test suite | Unit, integration and end-to-end test infrastructure and the quality gates built on it. |
| F24 | docker-compose deployment | One-command startup of the full stack. |
| F48 | Cortrix Agent | Bundled reference agent: REST + SSE middleware with a chat UI, so the engine can be tried without wiring up an external agent. |

### Product surfaces

| # | Name | What it is |
|---|---|---|
| P02a | Web UI | The self-hosted web interface under `web/`. |
| P03 | Python SDK | The `cortrix` PyPI package under `sdk/python/`. |
| P04 | API documentation | The OpenAPI 3.0 spec under `api/` as the single source of truth, including the `CX_ERR_*` error-code registry. |
| P08 | Auth system | Email + password accounts, one-time bootstrap URL, admin API keys, JWT. |
| P09 | Tenant management | Tenant model and namespace permission assignment. |
| P12 | MCP server | The `cortrix-mcp` package: Cortrix tools for MCP-compatible clients and IDE agents. |
| P14 | Skill SDK | The `cortrix-skills` package: the same tool surface as P12 exposed as framework adapters. |

### Referenced in the tree, not shipped in v1.0

These identifiers appear in comments and reserved seams but name work that is
not part of this release. They are indexed here only so the references can be
resolved; nothing in this subsection is a shipped capability.

| # | Name | Where the reference lives |
|---|---|---|
| F11 | Cleaning plugin API | v1.0 ships only the `ICleaningPlugin` seam and stub inside F10; the plugin ecosystem itself is unreleased. |
| F43 | Block hotness self-learning | Comments mark the hooks reserved for it; no shipped behavior. |
| F44 | Benchmark suite | The BEIR evaluation harness is maintained outside this repository; comments and a placeholder Dockerfile under `tests/benchmark/beir/` reference it. |
| P01 | Multi-tenancy and quota | Interface-reserved seams only (e.g. scatter-plan coordination); the capability is unreleased. |
