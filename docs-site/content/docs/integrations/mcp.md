---
title: MCP server
description: Let Claude Code, Cursor, and other MCP clients call Cortrix.
weight: 20
---

On this page you'll install `cortrix-mcp` and connect it to an MCP client, so the agent in that client can query, upload documents, and manage memory.

`cortrix-mcp` exposes Cortrix's HTTP API as 31 MCP tools: 29 regular tools and 2 admin tools. It talks HTTP directly to a running `cortrix-server`.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- Python 3.10 or later.
- An MCP client that supports stdio transport.

> [!NOTE]
> Only local stdio transport is supported. Cortrix doesn't expose a remote or loopback Streamable HTTP MCP endpoint.

## Install {#install}

Install the package from PyPI into a virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install cortrix-mcp
```

This gives you the `cortrix-mcp` command, which is the entry point for the MCP stdio server. Confirm the install:

```bash
pip list | grep -E '^(cortrix-mcp|mcp) '
```

```text
cortrix-mcp               1.0.0rc2
mcp                       2.3.0
```

> [!NOTE]
> Cortrix is in pre-release, so pip installs the latest pre-release, `1.0.0rc2`. The Docker image `cortrix/mcp` isn't published to a registry yet. To run the server in a container, build the image from `cortrix-mcp/` in a clone of the repository.

## Configuration {#configuration}

All configuration is passed through environment variables:

| Variable | Purpose | Default |
|---|---|---|
| `CORTRIX_URL` | The address of `cortrix-server` | `http://127.0.0.1:8420` |
| `CORTRIX_NAMESPACE` | The default namespace | `default` |
| `CORTRIX_API_KEY` | A Bearer token or API key for the target server | Empty |
| `CORTRIX_MCP_ADMIN` | Turns on admin tools when the server accepts the caller as an admin | `false` |
| `CORTRIX_MCP_TIMEOUT` | HTTP timeout, in seconds | `30` |
| `CORTRIX_AGENT_ID` | The agent identity sent as `X-Agent-Id` on every request. Allowed characters are `[A-Za-z0-9_.:/-]`, up to 128 characters. An invalid value falls back to the default | `cortrix-mcp` |

The quickstart service has no authentication, so you don't need `CORTRIX_API_KEY`. Its demo data lives in the `demo` namespace, so the examples below set the default namespace to `demo`.

## Connect a client {#connect-a-client}

### Claude Code {#claude-code}

Run this in your project directory. `cortrix-mcp` needs to be on your `PATH`. Otherwise, give the absolute path inside your virtual environment:

```bash
claude mcp add cortrix --scope project \
  --env CORTRIX_URL=http://127.0.0.1:8420 \
  --env CORTRIX_NAMESPACE=demo \
  -- cortrix-mcp
```

```text
Added stdio MCP server cortrix with command: cortrix-mcp  to project config
```

This creates `.mcp.json` in the current directory:

```json {title=".mcp.json"}
{
  "mcpServers": {
    "cortrix": {
      "type": "stdio",
      "command": "cortrix-mcp",
      "args": [],
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8420",
        "CORTRIX_NAMESPACE": "demo"
      }
    }
  }
}
```

Claude Code asks you to approve a project-scoped MCP server before using it. Start `claude`, approve `cortrix`, and then run `/mcp` to check the connection.

> [!TIP]
> `.mcp.json` can contain absolute paths from your machine. The Cortrix repository's `.gitignore` already ignores this file. Whether to commit it in your own project is up to you.

### Cursor {#cursor}

Add this to `~/.cursor/mcp.json`:

```json {title="~/.cursor/mcp.json"}
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8420",
        "CORTRIX_NAMESPACE": "demo"
      }
    }
  }
}
```

### Cline {#cline}

Add this to `cline_mcp_settings.json` in your VS Code user directory:

```json {title="cline_mcp_settings.json"}
{
  "mcpServers": {
    "cortrix": {
      "command": "cortrix-mcp",
      "env": {
        "CORTRIX_URL": "http://127.0.0.1:8420",
        "CORTRIX_NAMESPACE": "demo"
      },
      "disabled": false
    }
  }
}
```

## Verify {#verify}

Once connected, ask the agent to call `cortrix_health`. The result's `meta.category` should be `success`:

```json
{
  "data": { "status": "alive", "uptime_seconds": 195, "version": "1.0.0-rc.2" },
  "meta": {
    "retryable": false,
    "category": "success",
    "retry_after_ms": null,
    "structured_data": {
      "trace_id": "17912bb9-4f29-4a2f-9a35-e5ccd2ac28c8",
      "session_id": "mcp-session-da64d7e40fe9"
    }
  }
}
```

Then ask it to use `cortrix_query` to look up "What does semantic storage keep close to the agents that need it?". It should return content from `quickstart-demo.txt`.

## Tools {#tools}

| Group | Tools |
|---|---|
| Core | `cortrix_health`, `cortrix_query`, `cortrix_upload`, `cortrix_list_documents`, `cortrix_list_namespaces`, `cortrix_create_namespace`, `cortrix_memory_search`, `cortrix_log_interaction`, `cortrix_list_interactions`, `cortrix_document_status`, `cortrix_add_watcher`, `cortrix_list_watchers` |
| Extended | `cortrix_cross_ns_query`, `cortrix_async_upload`, `cortrix_memory_search_filter`, `cortrix_memory_extract_trigger` |
| Tasks and explain | `cortrix_memory_extract`, `cortrix_task_status`, `cortrix_cancel_task`, `cortrix_query_explain` |
| Memory and operations | `cortrix_memory_get_audit`, `cortrix_memory_revoke_fact`, `cortrix_memory_opt_out`, `cortrix_batch_submit`, `cortrix_list_operations`, `cortrix_memory_list`, `cortrix_memory_create`, `cortrix_memory_edit`, `cortrix_memory_invalidate` |
| Admin | `cortrix_admin_db_credential_register`, `cortrix_admin_db_import_run` |

For each tool's parameters and status, see [MCP tools](/docs/reference/mcp-tools/).

## Response schema {#response-schema}

Every tool returns two layers: `data` holds the result, and `meta` tells the agent whether the call can be retried.

When a tool, the backend, authentication, a timeout, or validation fails, the result comes back with `isError=true` and keeps `code`, `retryable`, `category`, `retry_after_ms`, and `structured_data`.

The adapter defines 6 stable error codes of its own:

| Error code | Retryable | Category | Suggested wait (ms) |
|---|:--:|---|:--:|
| `CX_ERR_MCP_BACKEND_TIMEOUT` | Yes | `transient` | 1000 |
| `CX_ERR_MCP_BACKEND_UNAVAILABLE` | Yes | `transient` | 5000 |
| `CX_ERR_MCP_SCHEMA_VALIDATION_FAIL` | No | `permanent` | — |
| `CX_ERR_MCP_TOOL_NOT_FOUND` | No | `permanent` | — |
| `CX_ERR_MCP_AUTH_MISSING` | No | `auth` | — |
| `CX_ERR_MCP_ADMIN_REQUIRED` | No | `auth` | — |

Errors that come from `cortrix-server` itself pass through unchanged.

## Trace a call {#tracing}

Every backend request carries `X-Session-Id`, a fresh `X-Trace-Id`, and `X-Agent-Id`. The server adopts these identifiers, so you can use the `session_id` in a tool's `meta.structured_data` to look up the server-side trace directly:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/traces/<session_id>
```

## Protocol compatibility {#protocol-compatibility}

- The modern protocol `2026-07-28`, through `server/discover`.
- The legacy protocol `2025-11-25`, through the initialize handshake.
- Python dependency: `mcp>=2.0.0,<3.0.0`.

## Next steps {#next-steps}

- [Python SDK](/docs/integrations/python-sdk/)
- [Compatibility and status](/docs/resources/compatibility/)
- [HTTP API reference](/docs/reference/api/)
