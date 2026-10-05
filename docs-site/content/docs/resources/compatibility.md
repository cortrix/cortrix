---
title: Compatibility and status
description: The current status of every capability.
weight: 10
---

Check here for the status of each Cortrix capability before you make a production, security, benchmark, or integration decision.

> [!IMPORTANT]
> Cortrix is in pre-release. The current version is `v1.0.0-rc.2`. Nothing in this documentation is a production-readiness commitment yet. Verify it in your own environment before you use it in production.

## What each status means {#status-labels}

| Status | Meaning |
|---|---|
| {{< badge text="Preview" tone="success" >}} | Available and documented. Verify it in your own environment before production use |
| {{< badge text="In development" tone="info" >}} | Present in part or being built. Not supported yet. Don't rely on it |

## Current status {#status-matrix}

### Preview {#preview}

| Capability | Documentation |
|---|---|
| Docker quickstart | [Quickstart](/docs/get-started/quickstart/) |
| Namespaces, document upload, and processing | [Upload documents](/docs/guides/upload-documents/) |
| Query, cross-namespace query, and reranking | [Query and rerank](/docs/guides/query/) |
| Writing, listing, editing, and deleting memories. Semantic search over explicit memories doesn't return results in `v1.0.0-rc.2` | [Work with memory](/docs/guides/memory/) |
| Correcting a memory by hand: invalidate, restore, and the audit trail | [Work with memory](/docs/guides/memory/#edit) |
| Directory watchers | [Watch a directory](/docs/guides/watch/) |
| The HTTP API and the OpenAPI spec | [HTTP API](/docs/reference/api/) |
| MCP server (local stdio) | [MCP server](/docs/integrations/mcp/) |
| Python SDK | [Python SDK](/docs/integrations/python-sdk/) |
| Framework adapters | [Framework adapters](/docs/integrations/skills/) |
| Fixed-flow chat in the built-in Agent | [Built-in Agent](/docs/integrations/agent/) |
| The NVIDIA CUDA execution provider on Linux | [NVIDIA CUDA](/docs/deploy/cuda/) |
| OCR and document parsers | — |

### In development {#in-development}

- Tenants, members, access control, and quotas
- Tenant isolation
- Memory extraction, including automatic handling of contradicting memories
- pgcortrix
- Database import
- Log redaction

## Using a preview capability {#using-preview}

An endpoint that appears in the OpenAPI spec is part of the documented API surface. That doesn't mean it has been verified in every runtime. When you write client code:

- Take request and response shapes from the OpenAPI spec. This documentation notes where tested behavior differs from the spec.
- Pin the version of your target server, and rerun your smoke tests after you upgrade Cortrix.

## Authentication and security {#auth-and-security}

Tenant isolation and access control are in development, and API key authentication hasn't been verified for this documentation. Until they're available, deploy Cortrix only on a network you control. See [Security boundaries](/docs/deploy/security/).

A namespace is a logical query scope. It isn't an authentication or tenant-security boundary.

## Benchmarks {#benchmark-boundary}

The published benchmark numbers measure retrieval quality at `top_k=10`. They say nothing about end-to-end answer quality, production latency or capacity under concurrency, security or compliance, how Cortrix ranks against other products, or business outcomes. For the measurement conditions and data, see [Benchmark evidence](/docs/resources/benchmarks/).

## Before production {#production-boundary}

At a minimum, verify these in your target environment:

- Data persistence and backup strategy
- Deployment topology and resource limits
- Network exposure
- Compatibility of the API, SDK, and MCP server with your target build
- The actual behavior of every preview capability you depend on
