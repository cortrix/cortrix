# Agent Access

Cortrix is designed to be used by humans, services, and Agents. This page explains the public access paths and their current support boundaries.

Read [Compatibility and known status](compatibility.md) before using any integration in production.

## Access Paths

| Path | Use when | Entry point | Current status |
|---|---|---|---|
| HTTP API / OpenAPI | You are building your own client, service, or Agent integration. | [OpenAPI spec](../api/openapi.yaml) | `RD review required` |
| MCP server | You want an IDE Agent or MCP-compatible client to call Cortrix tools. | [MCP README](../cortrix-mcp/README.md) | `RD review required` |
| Python SDK | You are building a Python app, RAG pipeline, or test harness. | [Python SDK README](../sdk/python/README.md) | `RD review required` |
| Built-in Agent | You want local fixed-flow chat over Cortrix storage. | [Built-in Agent README](../cortrix-agent/README.md) | `RD review required` |

## HTTP API / OpenAPI

Use the HTTP API when you want full control over requests, retries, auth, logging, and deployment.

Start with:

- [OpenAPI spec](../api/openapi.yaml)
- [Quickstart](QUICKSTART.md)
- [Compatibility](compatibility.md)

Example health check:

```bash
curl http://localhost:8420/api/v1/health
```

Example query:

```bash
curl -X POST http://localhost:8420/api/v1/query \
  -H "Content-Type: application/json" \
  -d '{
    "query": "What is Cortrix?",
    "namespace": "default",
    "top_k": 5
  }'
```

Auth schemes are defined in the OpenAPI spec:

- `X-API-Key`
- Bearer token

Auth login and several tenant/RBAC surfaces are currently not safe to label as verified. See [Compatibility](compatibility.md).

## MCP Server

Use MCP when your Agent client supports Model Context Protocol over stdio.

Setup starts in [cortrix-mcp/README.md](../cortrix-mcp/README.md).

Minimal environment:

```bash
export CORTRIX_URL=http://127.0.0.1:8420
export CORTRIX_NAMESPACE=default
export CORTRIX_API_KEY=your-cortrix-api-key
cortrix-mcp
```

The MCP package documents tool groups for health, query, upload, namespace management, memory, task status, watchers, and admin database import. Treat tool availability as `RD review required` until your target server/runtime is verified.

## Python SDK

Use the Python SDK when you want typed Python access to Cortrix resources.

Setup starts in [sdk/python/README.md](../sdk/python/README.md).

Minimal example:

```python
from cortrix import Cortrix

client = Cortrix(base_url="http://localhost:8420", api_key="your-cortrix-api-key")
health = client.system.health()
print(health)
client.close()
```

The SDK exposes resources for documents, namespaces, search, memory, watchers, auth, tenants, system, operations, and database import. Some resources map to API areas that are currently blocked or under RD review.

## Built-in Agent

Use the built-in Agent when you want local fixed-flow chat against Cortrix storage without wiring a separate Agent framework.

Setup starts in [cortrix-agent/README.md](../cortrix-agent/README.md).

Minimal health check:

```bash
curl http://localhost:8001/health
```

Chat mode is the current documented path. Advanced autonomous executor modes, including tool-use and plan-execute flows, are roadmap items and must not be described as current production capabilities.

## Choosing A Path

Use this rule of thumb:

- Choose HTTP/OpenAPI for precise control and language-independent integration.
- Choose MCP for IDE Agents and MCP-compatible automation.
- Choose Python SDK for Python-first application code.
- Choose the built-in Agent for local chat/RAG flows where a fixed flow is acceptable.

For any production claim, check [Compatibility](compatibility.md) and verify your target runtime.
