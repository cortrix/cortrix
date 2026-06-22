# cortrix-mcp

Cortrix MCP Server — exposes the Cortrix HTTP API as **29 MCP tools + 2 admin tools**
over the Model Context Protocol (stdio), so IDE agents like **Claude Code**, **Cline**,
and **Cursor** can index documents, run hybrid semantic search, manage conversation
memory, and trigger admin database imports.

> Status: `RD review required`. The MCP package and tool groups are documented and
> test-covered, but public-readiness labeling still depends on the target
> cortrix-server runtime and API compatibility. See
> [Agent access](../docs/agent-access.md) and
> [Compatibility](../docs/compatibility.md).

Built on [FastMCP](https://github.com/modelcontextprotocol) + [httpx](https://www.python-httpx.org/).
It talks HTTP directly to a running `cortrix-server` (no Python SDK in the path).

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
  cortrix/mcp:v1.0.0
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

> The server connects to cortrix-server on port **8420** by default.

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
        "cortrix/mcp:v1.0.0"
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

On error the tool raises an `McpError` (MCP `isError=true`) whose `data` carries the same
4 fields. There are 6 MCP protocol error codes:

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

AGPL-3.0-only.
