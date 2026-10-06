---
title: MCP tools
description: Every tool the MCP server provides, with its parameters.
weight: 20
---

`cortrix-mcp` provides 31 tools in `v1.0.0-rc.2`. The tool names and parameters come from the tool list returned by a running MCP server.

A `*` after a parameter name means it's required. A tool that isn't given a `namespace` uses the default namespace set by the `CORTRIX_NAMESPACE` environment variable.

For installation and setup, see [MCP server](/docs/integrations/mcp/).

> [!NOTE]
> Tools marked {{< badge text="In development" tone="info" >}} have no documentation yet.

## Health and namespaces {#health-and-namespaces}

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_health` | Checks that the Cortrix service is running and reachable | — |
| `cortrix_list_namespaces` | Lists the namespaces you have access to | `limit`, `offset` |
| `cortrix_create_namespace` | Creates a namespace | `name`\* |

## Query {#query}

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_query` | Runs a semantic query over one or more namespaces | `query`\*, `top_k`, `namespaces`, `rerank` |
| `cortrix_cross_ns_query` | Queries across several namespaces | `query`\*, `namespaces`\*, `top_k`, `rerank` |
| `cortrix_query_explain` | Runs a query and returns an explanation of the path it took | `query`\*, `top_k`, `namespaces`, `rerank` |

For `cortrix_query`, `top_k` defaults to 5 and `rerank` defaults to `true`.

## Documents {#documents}

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_upload` | Uploads document content for asynchronous processing and returns a task | `content`\*, `namespace`, `filename`, `metadata` |
| `cortrix_async_upload` | Uploads a larger document asynchronously and returns a task | `content`\*, `namespace`, `filename`, `metadata` |
| `cortrix_batch_submit` | Submits up to 100 documents to one namespace at once | `namespace`\*, `documents`\*, `async_`, `on_duplicate` |
| `cortrix_list_documents` | Lists the documents in a namespace | `namespace`, `limit`, `offset` |
| `cortrix_document_status` | Shows a document's processing status and details | `doc_id`\*, `namespace` |
| `cortrix_task_status` | Shows the progress of an asynchronous upload task | `task_id`\* |
| `cortrix_cancel_task` | Cancels an asynchronous upload task | `task_id`\* |

## Directory watchers {#watchers}

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_add_watcher` | Adds a watcher that imports the files in a directory automatically | `data_dir`\*, `namespace`, `recursive` |
| `cortrix_list_watchers` | Lists all watchers | — |

## Memory {#memory}

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_memory_create` | Creates a memory | `namespace`\*, `content`\*, `memory_type`\*, `metadata` |
| `cortrix_memory_list` | Lists memories | `namespace`, `memory_type`, `include_invalidated`, `limit`, `offset` |
| `cortrix_memory_edit` | Edits a memory's content or metadata | `memory_id`\*, `namespace`, `content`, `metadata` |
| `cortrix_memory_invalidate` | Soft-deletes a memory | `memory_id`\*, `namespace`, `reason` |
| `cortrix_memory_search` | Runs a semantic search over conversation memory | `query`\*, `namespace`, `top_k` |
| `cortrix_memory_search_filter` | Searches memory filtered by memory type | `query`\*, `memory_type`\*, `namespace`, `top_k` |
| `cortrix_memory_opt_out` | Opts a session out of memory, or revokes the opt-out | `session_id`\*, `opt_out`, `namespace`, `reason` |
| `cortrix_memory_get_audit` | Queries the memory audit records | `memory_id`, `namespace`, `limit` |

These tools are {{< badge text="In development" tone="info" >}}:

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_memory_extract` | Extracts memories from a conversation in one call | `messages`\*, `namespace` |
| `cortrix_memory_extract_trigger` | Triggers memory extraction for a session manually | `namespace`, `session_id` |
| `cortrix_memory_revoke_fact` | Revokes an automatically extracted fact | `memory_id`\*, `namespace` |

## Interactions and the operation log {#interactions-and-operations}

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_log_interaction` | Saves a conversation turn: the user's question and the assistant's answer | `session_id`\*, `query_text`\*, `response_text`\*, `namespace` |
| `cortrix_list_interactions` | Lists a user's interactions | `namespace`\*, `user_id`\*, `filter`, `limit`, `offset` |
| `cortrix_list_operations` | Queries the operation log | `user_id`, `namespace`, `action`, `action_in`, `start_time`, `end_time`, `limit`, `offset`, `sort_order` |

## Admin {#admin}

Admin tools require the server to accept the caller as an admin.

| Tool | What it does | Parameters |
|---|---|---|
| `cortrix_admin_db_credential_register` | Registers a database connection credential | `name`, `dsn`, `expire_days`, `connection_ref`, `description` |
| `cortrix_admin_db_import_run` | Starts a database import | `connection_ref`\*, `namespace`\*, `table`, `filter`, `sql` |

These two tools are {{< badge text="In development" tone="info" >}}.

## Responses and errors {#responses}

Every tool returns two layers, `data` and `meta`. For the response schema and the adapter's own error codes, see [MCP server](/docs/integrations/mcp/#response-schema).
