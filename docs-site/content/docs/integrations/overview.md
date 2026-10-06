---
title: Choose an access path
description: Compare the HTTP API, MCP, the Python SDK, and the built-in Agent.
weight: 10
---

Cortrix has four access paths. This page helps you pick the one that fits your situation.

## How to choose {#choosing}

| Your situation | Choose |
|---|---|
| You need precise control over requests, retries, authentication, and deployment, or you're not using Python | [HTTP API](#http-api) |
| You want an agent in your IDE, or another MCP-compatible client, to call Cortrix | [MCP server](#mcp) |
| You're writing a Python application, a RAG pipeline, or a test script | [Python SDK](#python-sdk) |
| You want local fixed-flow chat over what's in Cortrix, without wiring up an agent framework | [Built-in Agent](#built-in-agent) |

## HTTP API {#http-api}

Every other access path ends up calling the HTTP API. The contract is `api/openapi.yaml` in the repository. See the [HTTP API reference](/docs/reference/api/) for every endpoint.

Check that the service is ready:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

Run a query:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["demo"],
    "query": "What does semantic storage keep close to the agents that need it?",
    "top_k": 5,
    "rerank": true
  }' \
  http://127.0.0.1:8420/api/v1/query
```

When authentication is on, send your API key in the `X-API-Key` header.

## MCP server {#mcp}

The MCP server exposes Cortrix's HTTP API as a set of MCP tools covering health, query, upload, namespace management, memory, task status, directory watchers, and admin database import.

It supports local stdio transport only. There is no remote Streamable HTTP endpoint.

For setup, see [MCP server](/docs/integrations/mcp/).

## Python SDK {#python-sdk}

The Python SDK provides a synchronous client, `Cortrix`, and an asynchronous client, `AsyncCortrix`. Both are fully typed and map the server's structured errors to exceptions with fields.

It exposes resources for documents, namespaces, query, memory, directory watchers, authentication, tenants, system information, operations, and database import. For the status of each resource, see [Python SDK](/docs/integrations/python-sdk/#resources).

For installation and usage, see [Python SDK](/docs/integrations/python-sdk/).

## Built-in Agent {#built-in-agent}

The built-in Agent is a FastAPI service that provides fixed-flow chat over Cortrix storage. It needs the `agent_llm` role configured, and it's off by default in the quickstart.

Chat is the only mode.

See [Built-in Agent](/docs/integrations/agent/).

## Next steps {#next-steps}

- [MCP server](/docs/integrations/mcp/)
- [Python SDK](/docs/integrations/python-sdk/)
- [Compatibility and status](/docs/resources/compatibility/)
