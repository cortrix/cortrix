---
title: Built-in Agent
description: Run the fixed-flow local chat service.
weight: 50
---

The built-in Agent is a FastAPI service that provides fixed-flow RAG chat over Cortrix storage. It queries `cortrix-server` through the public Python SDK and has no privileged channel:

```text
Web UI chat -> Cortrix Agent (:8001) -> Python SDK -> cortrix-server (:8420)
                                    -> configured LLM provider
```

> [!NOTE]
> The setup steps and interface details on this page come from `cortrix-agent/README.md` in the repository. The built-in Agent needs a working LLM service, and it hasn't been tested for this page. The quickstart leaves the built-in Agent off by default.

## Current scope {#scope}

The built-in Agent has one mode: chat, a fixed RAG flow with no autonomous tool selection. It's in preview.

Persisting configuration through `PUT /config/agent_llm` hasn't been verified for this documentation.

## Prerequisites {#prerequisites}

- A running Cortrix service.
- Python 3.
- Credentials for an LLM service, for the `agent_llm` role.

## Start the service {#setup}

```bash
cd cortrix-agent
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
.venv/bin/uvicorn main:app --port 8001 --reload
```

Put your LLM service configuration in `.env`. Keep real keys only in ignored local files such as `.env`, and don't commit them.

## Health check {#health}

```bash
curl http://localhost:8001/health
```

The expected response shape:

```json
{
  "status": "ready",
  "cortrix_server": "unknown",
  "llm_reachable": true
}
```

In `v1.0.0-rc.2`, `cortrix_server` is always `"unknown"`: this endpoint doesn't check the Cortrix server. Use the server's own readiness endpoint for that. If `llm_reachable` is `false`, check the LLM service configuration.

## Chat interface {#chat}

`POST /chat` streams the answer over server-sent events:

```bash
curl -N -X POST 'http://localhost:8001/chat?explain=true' \
  -H 'Content-Type: application/json' \
  -H 'X-Cortrix-Namespace: default' \
  -d '{"message": "find privacy documents", "session_id": "s-001"}'
```

| Input | Location | Notes |
|---|---|---|
| `message` | Request body | The user message |
| `session_id` | Request body | An optional session identifier |
| `Authorization` | Header | An optional Bearer token, depending on the server's authentication mode |
| `X-Cortrix-Tenant-Id` | Header | An optional tenant identifier |
| `X-Cortrix-Namespace` | Header | Optional. Names the namespace |
| `explain=true` | Query parameter | Returns extra explanation metadata |
| `debug=true` | Query parameter | Returns more detail on failure |

Example SSE frames:

```text
data: {"chunk": "Based on"}
data: {"chunk": " the retrieved documents..."}
data: {"meta": {"session_id": "s-001", "chunk_ids": [], "rag_status": "success"}}
data: [DONE]
```

Errors come back as structured SSE error events with a code, a message, retryability, a category, an optional retry delay, and structured data.

## Other endpoints {#other-endpoints}

| Endpoint | Purpose |
|---|---|
| `GET /sessions/{id}` | Returns the recent turns a session keeps in memory |
| `GET /config` | Returns the current LLM configuration with the API key masked |
| `GET /config/providers` | Returns the LLM provider catalog used by the configuration UI |
| `PUT /config/agent_llm` | Updates the LLM configuration. Persisting it isn't a verified capability yet |
| `GET /health` | Returns service status, backend reachability, and LLM reachability |

## Next steps {#next-steps}

- [Choose an access path](/docs/integrations/overview/)
- [Configuration](/docs/deploy/configuration/): configure LLMs by role.
- [Compatibility and status](/docs/resources/compatibility/)
