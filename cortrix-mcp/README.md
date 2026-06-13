# cortrix-mcp

Cortrix MCP Server — exposes the Cortrix HTTP API as **29 MCP tools + 2 admin tools**
over the Model Context Protocol (stdio), so IDE agents like **Claude Code**, **Cline**,
and **Cursor** can index documents, run hybrid semantic search, manage conversation
memory, and trigger admin database imports.

Built on [FastMCP](https://github.com/modelcontextprotocol) + [httpx](https://www.python-httpx.org/).
It talks HTTP directly to a running `cortrix-server` (no Python SDK in the path).

## Why MCP

`cortrix-mcp` is the **IDE path** of the Cortrix 3-path agent-access strategy
(MCP for IDEs / Skill SDK for agent frameworks / Python SDK for direct API). Every tool
response carries the GEN-Agent two-layer schema so agents can reason about retries and
data integrity (see [Response schema](#response-schema)).

## Install

### pip

```bash
pip install cortrix-mcp
```

This installs the `cortrix-mcp` console command (the MCP stdio server entry point).

### Docker

```bash
docker run -i --rm \
  -e CORTRIX_URL=http://host.docker.internal:8080 \
  -e CORTRIX_API_KEY=sk-cortrix-... \
  cortrix/mcp:v1.0.0
```

## Configuration

All configuration is via environment variables:

| Variable | Purpose | Default |
|---|---|---|
| `CORTRIX_URL` | cortrix-server base URL | `http://127.0.0.1:8080` |
| `CORTRIX_NAMESPACE` | default namespace | `default` |
| `CORTRIX_API_KEY` | Bearer token (P08 API key) | *(empty)* |
| `CORTRIX_MCP_ADMIN` | admin override for the 2 admin tools (P08 role fallback) | `false` |
| `CORTRIX_MCP_TIMEOUT` | HTTP timeout in seconds | `30` |

> The server connects to cortrix-server on port **8080** by default (matching the
> server's `config.h` default and the MVP).

## IDE configuration examples

### Claude Code

`~/.config/Claude/claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8080",
        "CORTRIX_API_KEY": "sk-cortrix-..."
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
        "-e", "CORTRIX_URL=http://host.docker.internal:8080",
        "-e", "CORTRIX_API_KEY=sk-cortrix-...",
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
        "CORTRIX_URL": "http://127.0.0.1:8080",
        "CORTRIX_API_KEY": "sk-cortrix-..."
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
        "CORTRIX_URL": "http://127.0.0.1:8080",
        "CORTRIX_API_KEY": "sk-cortrix-..."
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

Every tool returns the **GEN-Agent two-layer 4-field** envelope:

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

## Development

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -e '.[test]'
pytest --cov=cortrix_mcp
```

## License

AGPL-3.0-only.
