# cortrix-mcp

Cortrix MCP Server — exposes the Cortrix HTTP API as **29 MCP tools + 2 admin tools**
over the Model Context Protocol (stdio), so IDE agents like **Claude Code**, **Cline**,
and **Cursor** can index documents, run hybrid semantic search, manage conversation
memory, and trigger admin database imports.

> Status: `Verification required`. The MCP package and tool groups are documented and
> test-covered, but public-readiness labeling still depends on the target
> cortrix-server runtime and API compatibility. See
> [Agent access](../docs/agent-access.md) and
> [Compatibility](../docs/compatibility.md).

Built on the official MCP Python SDK v2 `MCPServer` + [httpx](https://www.python-httpx.org/).
It talks HTTP directly to a running `cortrix-server` (no Python SDK in the path).

The current MCP transport is local stdio only. The same server supports modern
MCP `2026-07-28` through `server/discover` and the legacy `2025-11-25`
initialize handshake.

## Why MCP

`cortrix-mcp` is the IDE-oriented path for Agent access. Every tool response
uses a structured data/meta envelope so agents can reason about retries and data
integrity (see [Response schema](#response-schema)).

Use MCP when your client supports Model Context Protocol over stdio. Use the
Python SDK or direct HTTP API when you need application-level control over
transport, retries, deployment, or custom auth handling.

## Install

### pip

```bash
pip install cortrix-mcp
```

This installs the `cortrix-mcp` console command (the MCP stdio server entry point).

### Docker

```bash
docker run -i --rm \
  -e CORTRIX_URL=http://host.docker.internal:8420 \
  -e CORTRIX_API_KEY=your-cortrix-api-key \
  cortrix/mcp:v1.0.0-rc.2
```

## Configuration

All configuration is via environment variables:

| Variable | Purpose | Default |
|---|---|---|
| `CORTRIX_URL` | cortrix-server base URL | `http://127.0.0.1:8420` |
| `CORTRIX_NAMESPACE` | default namespace | `default` |
| `CORTRIX_API_KEY` | Bearer token or API key for the target server | *(empty)* |
| `CORTRIX_MCP_ADMIN` | Enables admin-only tools when the server accepts the caller as admin | `false` |
| `CORTRIX_MCP_TIMEOUT` | HTTP timeout in seconds | `30` |
| `CORTRIX_AGENT_ID` | Agent identity sent as `X-Agent-Id` on every backend request (charset `[A-Za-z0-9_.:/-]`, max 128 chars; invalid values fall back to the default) | `cortrix-mcp` |

> The server connects to cortrix-server on port **8420** by default.

### Observability

Every backend request carries `X-Session-Id` (a stable process-scoped
correlation id retained under its compatibility header name), a fresh
`X-Trace-Id`, and `X-Agent-Id`. cortrix-server adopts these ids for its
server-side traces, so the `session_id` / `trace_id` returned in each tool's
`meta.structured_data` can be looked up directly via
`GET /api/v1/traces/{session_id}` for cross-agent debugging and provenance
inspection. This correlation id is independent of MCP protocol negotiation.

Use placeholder values in examples. Do not commit real API keys.

## IDE configuration examples

### Claude Code

`~/.config/Claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8420",
        "CORTRIX_API_KEY": "your-cortrix-api-key"
      }
    }
  }
}
```

Docker variant:

```json
{
  "mcpServers": {
    "cortrix": {
      "command": "docker",
      "args": [
        "run", "-i", "--rm",
        "-e", "CORTRIX_URL=http://host.docker.internal:8420",
        "-e", "CORTRIX_API_KEY=your-cortrix-api-key",
        "cortrix/mcp:v1.0.0-rc.2"
      ]
    }
  }
}
```

### Cline (VS Code extension)

`cline_mcp_settings.json` (in your VS Code user directory):

```json
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8420",
        "CORTRIX_API_KEY": "your-cortrix-api-key"
      },
      "disabled": false
    }
  }
}
```

### Cursor

`~/.cursor/mcp.json`:

```json
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8420",
        "CORTRIX_API_KEY": "your-cortrix-api-key"
      }
    }
  }
}
```

## Tools

### Core (12)

`cortrix_health` · `cortrix_query` · `cortrix_upload` · `cortrix_list_documents` ·
`cortrix_list_namespaces` · `cortrix_create_namespace` · `cortrix_memory_search` ·
`cortrix_log_interaction` · `cortrix_list_interactions` · `cortrix_document_status` ·
`cortrix_add_watcher` · `cortrix_list_watchers`

### Extended (4)

`cortrix_cross_ns_query` · `cortrix_async_upload` · `cortrix_memory_search_filter` ·
`cortrix_memory_extract_trigger`

### New (4)

`cortrix_memory_extract` · `cortrix_task_status` · `cortrix_cancel_task` ·
`cortrix_query_explain`

### Memory & operations (9)

`cortrix_memory_get_audit` · `cortrix_memory_revoke_fact` · `cortrix_memory_opt_out` ·
`cortrix_batch_submit` · `cortrix_list_operations` · `cortrix_memory_list` ·
`cortrix_memory_create` · `cortrix_memory_edit` · `cortrix_memory_invalidate`

### Admin (2 — require `role=admin`)

`cortrix_admin_db_credential_register` · `cortrix_admin_db_import_run`

## Response schema

Every tool returns a structured two-layer envelope:

```json
{
  "data": { "...": "business data" },
  "meta": {
    "retryable": false,
    "category": "success",
    "retry_after_ms": null,
    "structured_data": { "trace_id": "...", "session_id": "...", "coverage_ratio": 1.0 }
  }
}
```

Tool, backend, auth, timeout, and validation failures return a model-visible
`CallToolResult` with `isError=true`. Both its text content and
`structuredContent` retain `code`, `retryable`, `category`, `retry_after_ms`,
and `structured_data`. `MCPError` is reserved for JSON-RPC and protocol
failures handled by the MCP host.

The adapter defines 6 stable tool error codes:

| Code | retryable | category | retry_after_ms |
|---|:--:|---|:--:|
| `CX_ERR_MCP_BACKEND_TIMEOUT` | true | transient | 1000 |
| `CX_ERR_MCP_BACKEND_UNAVAILABLE` | true | transient | 5000 |
| `CX_ERR_MCP_SCHEMA_VALIDATION_FAIL` | false | permanent | — |
| `CX_ERR_MCP_TOOL_NOT_FOUND` | false | permanent | — |
| `CX_ERR_MCP_AUTH_MISSING` | false | auth | — |
| `CX_ERR_MCP_ADMIN_REQUIRED` | false | auth | — |

Business errors from cortrix-server (`CX_ERR_NS_*`, `CX_ERR_MEM03_*`, …) pass through
unchanged.

## Compatibility Notes

- Python dependency: `mcp>=2.0.0,<3.0.0`.
- Modern protocol: `2026-07-28` via `server/discover`.
- Legacy protocol: `2025-11-25` via the initialize handshake.
- Transport: local stdio. Cortrix does not currently expose a remote or
  loopback Streamable HTTP MCP endpoint.
- MEM02 memory extraction is currently `Blocked` in the latest public status
  baseline because the runtime verification found an LLM transport timeout path.
- Auth, tenant/member/ACL/quota, RBAC, and tenant isolation behavior must be
  checked against [Compatibility](../docs/compatibility.md) before making a
  production or security claim.
- Tool count and tool names describe this package surface. They do not prove that
  every backend route is verified in every runtime.

## Development

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -e '.[test]'
pytest --cov=cortrix_mcp
```

## License

Apache-2.0. Historical `v1.0.0-rc.1` release artifacts remain under `AGPL-3.0-only`.
