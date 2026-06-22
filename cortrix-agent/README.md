# Cortrix Agent

Cortrix Agent is the built-in FastAPI service for fixed-flow chat over Cortrix
semantic storage. It uses the public Python SDK path to query cortrix-server and
does not use a privileged backend channel.

> Status: `RD review required`. Fixed-flow chat mode is documented, but
> deployment, LLM provider behavior, and API compatibility should be verified
> against your target runtime. See [Agent access](../docs/agent-access.md) and
> [Compatibility](../docs/compatibility.md).

```text
Web UI chat -> Cortrix Agent (:8001) -> Python SDK -> cortrix-server (:8420)
                                    -> configured LLM provider
```

The current public path is chat mode: a fixed RAG flow with no autonomous tool
selection. Advanced tool-use and plan-execute modes are roadmap items.

## Layout

| Path | Role |
|---|---|
| `agent_core/` | UI-agnostic chat, retrieval, prompt, error, explanation, and session logic |
| `routes/` | FastAPI routes for chat, sessions, and configuration |
| `llm/` | LLM provider adapter interface and implementations |
| `main.py` | FastAPI app assembly and startup sequence |
| `config.py` | Pydantic settings |

## Setup

```bash
cd cortrix-agent
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
.venv/bin/uvicorn main:app --port 8001 --reload
```

Use placeholder values in documentation and real provider keys only in local
ignored files such as `.env`.

## Health Check

```bash
curl http://localhost:8001/health
```

Expected response shape:

```json
{
  "status": "ok",
  "cortrix_server": true,
  "llm_reachable": true
}
```

If `cortrix_server` or `llm_reachable` is false, check backend startup and LLM
provider configuration.

## Chat API

### POST /chat

Streams a chat response over server-sent events.

```bash
curl -N -X POST 'http://localhost:8001/chat?explain=true' \
  -H 'Content-Type: application/json' \
  -H 'X-Cortrix-Namespace: default' \
  -d '{"message": "find privacy documents", "session_id": "s-001"}'
```

Common request inputs:

| Input | Location | Notes |
|---|---|---|
| `message` | JSON body | User message |
| `session_id` | JSON body | Optional session identifier |
| `Authorization` | Header | Optional Bearer token, depending on server auth mode |
| `X-Cortrix-Tenant-Id` | Header | Optional tenant identifier |
| `X-Cortrix-Namespace` | Header | Optional namespace override |
| `explain=true` | Query | Includes additional explanation metadata |
| `debug=true` | Query | Includes additional failure detail |

Example SSE frames:

```text
data: {"chunk": "Based on"}
data: {"chunk": " the retrieved documents..."}
data: {"meta": {"session_id": "s-001", "chunk_ids": [], "rag_status": "success"}}
data: [DONE]
```

Errors are returned as structured SSE error events with code, message,
retryability, category, optional retry delay, and structured data.

### GET /sessions/{id}

Returns the in-memory windowed history for a session.

### GET /config

Returns the current Agent LLM configuration with the API key masked.

### GET /config/providers

Returns the provider catalog used by the Agent configuration UI.

### PUT /config/agent_llm

Updates Agent LLM configuration when the runtime supports the required admin
path. Live persistence is not a current verified capability.

### GET /health

Returns service status, backend reachability, and LLM reachability.

## Compatibility Notes

- Built-in Agent chat is the current documented path.
- Advanced autonomous executor modes are `Roadmap`.
- Live persistence for `PUT /config/agent_llm` is not a current verified
  capability.
- Auth and tenant/RBAC claims should follow
  [Compatibility](../docs/compatibility.md).

## Tests

```bash
.venv/bin/pytest -q
```
