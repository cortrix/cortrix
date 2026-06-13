# Cortrix Agent (F48)

Cortrix's built-in Agent — a FastAPI middleware that lets a user chat with their Cortrix
semantic storage without configuring any external agent. It dogfoods the public access
strategy: RAG runs through the **P03 Python SDK** exactly like any third-party agent (no
privileged channel). This is the Cortrix Agent, not a "chatbot".

```
Web UI (Chat)  ->  Cortrix Agent (FastAPI, :8001 internal)  ->  P03 SDK  ->  cortrix-server
                          |
                          +->  LLM provider (OpenAI / Claude / Ollama / GLM / Mock; DeepSeek in V1.5)
```

Phase 1 V1.0 ships the **chat mode** (`ChatExecutor`): a fixed RAG flow with no
autonomous tool selection. `ToolUseExecutor` (V1.5) and `PlanExecuteExecutor` (V2) are
reserved behind the shared `IAgentExecutor` interface.

## Layout

| Path | Role |
|------|------|
| `agent_core/` | UI-agnostic kernel: `executor` (ChatExecutor + L1/L2/L3 degradation), `sdk_rag` (P03 SDK RAG seam), `prompt` (injection-hardened), `explain` (A/B/C meta tiers), `errors` (GEN-Agent 4-field), `session_store` (in-memory N=10 window) |
| `routes/` | HTTP layer: `chat` (SSE), `sessions`, `config` — thin encoders over `agent_core` |
| `llm/` | LLM adapter interface + implementations |
| `main.py` | FastAPI assembly + dependency injection + startup sequence (design §11.bis) |
| `config.py` | Pydantic settings (4-layer priority, design §6.1) |

## Setup

```bash
cd cortrix/cortrix-agent
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # add -i <mirror> if pip is slow
cp .env.example .env                          # then edit LLM keys

# Start (mock mode — no LLM key needed; cortrix-server not required for boot)
.venv/bin/uvicorn main:app --port 8001 --reload
```

## API (design §9.1)

### POST /chat — SSE streaming

```bash
curl -N -X POST 'http://localhost:8001/chat?explain=true' \
  -H 'Content-Type: application/json' \
  -H 'X-Cortrix-Tenant-Id: ' \
  -H 'X-Cortrix-Namespace: default' \
  -d '{"message": "find privacy documents", "session_id": "s-001"}'
```

Headers: `Authorization` (optional Bearer; not validated in CE), `X-Cortrix-Tenant-Id`
(optional, P-5 forward-compat), `X-Cortrix-Namespace` (optional, P-1 request-level NS
override). Query: `?explain=true` exposes B-class meta; `?debug=true` exposes C-class
failure detail.

SSE frames:

```
data: {"chunk": "Based on"}
data: {"chunk": " the retrieved documents..."}
data: {"meta": {"session_id": "s-001", "chunk_ids": [...], "latency_ms": {...},
                "rag_status": "success", "tenant_id": null, ...}}
data: [DONE]
```

`meta` tiers (design §1.7): **A** (always) `session_id` / `tenant_id` / `chunks_used` /
`chunk_ids` / `latency_ms` / `rag_status`; **B** (`?explain=true`) `prompt_text` /
`rag_chunks_full` / `model_used` / `llm_token_count`; **C** (`?debug=true` on failure)
`*_call_failed_detail`. Errors use the GEN-Agent 4-field envelope as an SSE `error`
event (`{error:{code, message, retryable, category, retry_after_ms, structured_data}}`).

### GET /sessions/{id}

Returns the in-memory windowed history (`messages` / `window_size` / `created_at` /
`tenant_id`); missing session -> `CX_ERR_F48_SESSION_NOT_FOUND` (404).

### GET /config, GET /config/providers, PUT /config/agent_llm

`GET /config` returns the current agent LLM config with the API key masked.
`GET /config/providers` returns the provider catalog reused by the P02a Settings UI.
`PUT /config/agent_llm` is admin-only; live persistence is `TODO(D3.5)` (cortrix-server
IGlobalConfig admin endpoint).

### GET /health

`{status, cortrix_server, llm_reachable}` (design §9.1).

## Tests

```bash
.venv/bin/pytest -q
```

## V1.5 / D3.5 scope (not in this V1.0 ①-kernel round)

- DeepSeek adapter (catalog lists it; adapter ships in V1.5)
- MEM co-processing: F13 interaction_log write + MEM02 extract trigger + MEM05 user_id
  filter (design F48-rev-9/10/11)
- Live cortrix-server health ping (§11.bis Step 4) + IGlobalConfig admin getter/setter
- `ToolUseExecutor` (V1.5) / `PlanExecuteExecutor` (V2)
