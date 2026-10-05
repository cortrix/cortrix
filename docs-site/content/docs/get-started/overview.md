---
title: Overview
description: What Cortrix is, what it's made of, and where it stands today.
weight: 10
---

Cortrix is open source semantic storage built for AI agents. It's agent-native storage for retrieval, memory, and API-driven AI applications.

It provides a semantic storage server with namespaces, documents, blocks, hybrid search, memory APIs, and agent-oriented access paths. It's designed for agents and applications that need a programmable retrieval layer rather than a one-off vector database wrapper.

> [!IMPORTANT]
> Cortrix is in active pre-release. The current version is `v1.0.0-rc.2`. Interfaces and features are at different levels of maturity, and authentication, authorization, and production readiness are not established. See [Compatibility and status](/docs/resources/compatibility/).

## What is semantic storage? {#what-is-semantic-storage}

Semantic storage connects information with the context an application needs to use it. Cortrix does this through retrieval, memory records, and links back to sources.

That maps to three needs in agent work:

| Need | What it means |
|---|---|
| Agent workflows | An agent needs to read source material, use tools, and keep track of what happened |
| Context for a task | A useful answer needs relevant material at the right moment. Storage keeps that material available, and retrieval decides which passages belong in the prompt |
| Memory across tasks | Some experience is worth carrying into the next task. Storage preserves it, but the system still needs to check whether an old observation applies |

## Design {#design}

- **Agent-native**: designed around agent data workflows. Semantic processing, memory, and traceability are shared storage concerns rather than scattered integration code.
- **Semantic storage**: documents are parsed, chunked, embedded, and indexed in a shared semantic layer, so agent workflows can query meaning and source context together.
- **Built to fit existing stacks**: works with your existing databases, tools, and agent workflows while giving retrieval, memory, and audit a shared semantic layer.

## Building blocks {#building-blocks}

| Building block | What it covers |
|---|---|
| Semantic Processing Chain (SPC) | Parsing, OCR fallback, chunking, enrichment, embedding, and indexing |
| Hybrid query and reranker | Vector search, BM25, rank fusion, and reranking |
| AI interaction memory | Memory APIs and typed memory records |
| Agent observability | Session, trace, and agent identity |
| Source-level traceability | Inspectable source context across chunks, documents, and turns |
| Namespaces and cross-namespace query | Logical query scope for projects and workflows |
| Local embedding | ONNX Runtime and BGE-M3 |
| Access paths | HTTP, MCP, the Python SDK, framework adapters, data import, and PostgreSQL |

> [!NOTE]
> A namespace is a logical query scope. It is not an authentication, authorization, access-control, or tenant-security boundary.

## Components in the repository {#components}

| Component | Location | Role |
|---|---|---|
| `cortrix-server` | `src/` | The C++ backend and HTTP API |
| OpenAPI spec | `api/openapi.yaml` | The public HTTP API contract |
| MCP server | `cortrix-mcp/` | An MCP server for IDE and agent clients |
| Python SDK | `sdk/python/` | The Python client |
| Built-in Agent | `cortrix-agent/` | A fixed-flow RAG chat service |
| Web UI | `web/` | The local web interface |

## Access paths {#access-paths}

| Path | Best for |
|---|---|
| HTTP API | Your own services, or direct calls from an agent |
| MCP server | IDE agents and MCP-compatible clients |
| Python SDK | Python applications and RAG pipelines |
| Built-in Agent | Local fixed-flow chat over Cortrix storage |

To pick one, see [Choose an access path](/docs/integrations/overview/).

## Current status {#status}

Cortrix is in pre-release. To see whether a capability is in preview or in development, see [Compatibility and status](/docs/resources/compatibility/).

## License {#license}

Cortrix Core is licensed under Apache-2.0. That applies to `v1.0.0-rc.2` and later. The historical `v1.0.0-rc.1` release remains under AGPL-3.0-only. Third-party material keeps its own license.

## Next steps {#next-steps}

- [Quickstart](/docs/get-started/quickstart/): start Cortrix with Docker and run your first query.
- [Install with an AI agent](/docs/get-started/agent-quickstart/): hand setup and verification to an AI agent.
- [Core concepts](/docs/get-started/core-concepts/): learn about namespaces, documents, blocks, queries, and memory.
