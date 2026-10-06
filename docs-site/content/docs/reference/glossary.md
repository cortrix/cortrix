---
title: Glossary
description: The terms used in the Cortrix documentation.
weight: 60
---

These are the terms the Cortrix documentation uses. Each concept has exactly one name.

## Product and positioning {#product}

| Term | Definition |
|---|---|
| access path | A way for an agent or application to use Cortrix: the HTTP API, MCP, the Python SDK, framework adapters, or the built-in Agent |
| Agent-native | Designed around agent data workflows: semantic processing, memory, and traceability are concerns of the shared storage layer |
| built-in Agent | The fixed-flow chat service that ships in the repository |
| framework adapter | The `cortrix-skills` package, which turns Cortrix tools into each agent framework's native format |
| Quick Start | The path that starts Cortrix locally with Docker and runs a first query |
| semantic storage | A storage layer that connects information with the context an application needs to use it |

## Data model {#data-model}

| Term | Definition |
|---|---|
| block | The smallest searchable unit, derived from a document or a memory record |
| document | Source material uploaded to Cortrix |
| event | A memory type. It decays over time |
| fact | A memory type. It doesn't decay over time |
| interaction | One conversation turn: the user's question and the assistant's answer |
| memory | Structured long-term information. Its type is fact, preference, or event |
| metadata | Key-value information attached to a document, block, or memory |
| namespace | A logical collection boundary for documents, blocks, memory, and queries. It isn't an authentication or tenant-security boundary |
| preference | A memory type. It doesn't decay over time |
| session | A set of conversation turns grouped together |
| source | The original document a block belongs to |

## Ingestion {#ingestion}

| Term | Definition |
|---|---|
| batch submit | Submitting several documents in one request |
| chunking | Splitting text into blocks |
| database import | Importing the rows of a database table or query into a namespace |
| embedding | Turning text into a vector |
| enrichment | Using an LLM to add entities and summaries to ingested content |
| file upload | Uploading one file as multipart. It returns the document identifier right away |
| ingestion | Everything that happens to a document between upload and being searchable |
| inline upload | Submitting text content directly in a JSON request body. Processing is asynchronous |
| parsing | Turning a file into text |
| Semantic Processing Chain (SPC) | The ingestion pipeline: parsing, chunking, enrichment, embedding, and indexing |
| task | One asynchronous processing job. You can check its progress and cancel it |
| watcher | Tracks a directory and imports new files in it automatically |

## Retrieval {#retrieval}

| Term | Definition |
|---|---|
| candidate pool | The set of candidates fetched before reranking and truncation. It's larger than `top_k` |
| coverage ratio | The share of requested namespaces that were queried successfully |
| cross-namespace query | Querying several namespaces at once and merging the results |
| dense retrieval | Looking up semantically similar content in the vector index |
| full-text search | Looking up content by keyword |
| fusion | Merging the results of several retrieval paths into one list by rank |
| hybrid search | Using several retrieval paths together and fusing their results |
| partial success | Returning the parts that succeeded when some sub-operations fail, with the failures described in `meta` |
| query | A retrieval request over one or more namespaces |
| rerank score | The relevance score given by the reranker model |
| reranking | Scoring each candidate with a cross-encoder model and reordering them |
| sparse retrieval | Looking up content with sparse vectors |

## Memory operations {#memory-operations}

| Term | Definition |
|---|---|
| invalidation | Marking a memory as no longer valid while keeping the record |
| memory extraction | Extracting memories from conversations automatically |
| soft delete | Marking something deleted while keeping it restorable or traceable, without physically removing it |

## Deployment and runtime {#deployment}

| Term | Definition |
|---|---|
| admin endpoint | A management interface that accepts loopback requests only |
| execution provider | The backend that runs a model: CPU, CoreML, or CUDA |
| garbage collection | The background process that removes deleted data in stages |
| LLM role | One use of an LLM, configured separately. There are five: `semantic_llm`, `vision_llm`, `agent_llm`, `doc_summary_llm`, and `enricher_llm` |
| local model | The embedding and reranking models that run on your machine and need no external service |
| loopback address | A network address reachable only from the same machine |
| model manifest | The file that records each model file's source, size, and checksum |
| readiness check | The health check that confirms the API, the models, and the data are all available |
| volume | The Docker storage that holds persistent data |

## Interfaces {#interfaces}

| Term | Definition |
|---|---|
| authentication | Confirming who the caller is |
| error category | The classification of an error: `auth`, `quota`, `transient`, `timeout`, or `permanent` |
| structured error | An error response that carries `code`, `retryable`, `category`, and `retry_after_ms` |
| tenant | One unit of isolation in the multi-tenant model |
| tool | A callable operation that the MCP server or the framework adapters offer to an agent |
| trace | The record a call leaves on the server, which can be queried by session |

## Status labels {#status-labels}

| Term | Definition |
|---|---|
| In development | Present in part or being built. Not supported yet |
| Preview | Available and documented. Cortrix is in pre-release, so verify it in your own environment before production use |
