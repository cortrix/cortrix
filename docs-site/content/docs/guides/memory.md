---
title: Work with memory
description: Write, list, edit, and delete memories.
weight: 30
---

On this page you'll write memories explicitly through the API, then list, edit, and delete them.

> [!NOTE]
> The output on this page is real output from `v1.0.0-rc.2`. It differs from the OpenAPI spec in several places: a memory's `status` is `active` where the spec lists `valid` and `invalidated`; the default `page_size` is 50 where the spec says 20; and the responses to editing and deleting a memory have a different shape, and need an `ns` parameter the spec doesn't declare.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- A namespace to hold the memories. This page uses `assistant`:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"name": "assistant", "display_name": "Assistant memory"}' \
  http://127.0.0.1:8420/api/v1/namespaces
```

## Memory types {#memory-types}

| Type | Meaning |
|---|---|
| `fact` | A fact |
| `preference` | A preference |
| `event` | An event. Event memories decay over time, while facts and preferences don't |

## Write a memory {#create}

`namespace`, `content`, and `memory_type` are required. `metadata` is optional:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespace": "assistant",
    "content": "User preference: use Markdown for report format",
    "memory_type": "preference"
  }' \
  http://127.0.0.1:8420/api/v1/memory
```

```json
{ "memory_id": "01M421CJRVM3KE0VYK5V8Y0753", "status": "active" }
```

A memory written this way is an explicit memory. It isn't validated by an LLM.

## List memories {#list}

```bash
curl -fsS 'http://127.0.0.1:8420/api/v1/memory?namespace=assistant'
```

```json
{
  "memories": [
    {
      "content": "User preference: use Markdown for report format",
      "extracted_at": 1791069932315,
      "memory_id": "01M421CJRVM3KE0VYK5V8Y0753",
      "memory_type": "preference",
      "status": "active"
    }
  ],
  "page": 0,
  "page_size": 50,
  "total": 1
}
```

Query parameters:

| Parameter | Description |
|---|---|
| `namespace` | Required |
| `memory_type` | List only one type: `fact`, `preference`, or `event` |
| `include_invalidated` | When `true`, also return invalidated memories. The default is `false` |
| `limit`, `offset` | Paging |

## Edit a memory {#edit}

An edit doesn't overwrite the original. Cortrix creates a new memory and marks the old one invalidated:

```bash
curl -fsS -X PATCH -H 'Content-Type: application/json' \
  -d '{
    "ns": "assistant",
    "content": "User preference: use Markdown for reports and include a summary table"
  }' \
  http://127.0.0.1:8420/api/v1/memory/01M421CJRVM3KE0VYK5V8Y0753
```

```json
{
  "invalidated_memory_id": "01M421CJRVM3KE0VYK5V8Y0753",
  "new_memory_id": "01M421CWYEK961J68WZ9KTNVS6"
}
```

> [!WARNING]
> The edit request body must include both `ns` and `content`, or the request returns 400 `ns + content are required`. The OpenAPI spec doesn't list `ns` in this endpoint's request body, so this page follows the tested behavior.

## Delete a memory {#delete}

Deleting a memory is a soft delete. The memory is marked `invalidated`, but it's always kept and never physically removed. Pass the namespace as the `ns` query parameter:

```bash
curl -fsS -X DELETE \
  'http://127.0.0.1:8420/api/v1/memory/01M421CJTK0AQ19B9W4ZMWB3S4?ns=assistant'
```

```json
{ "block_id": "01M421CJTK0AQ19B9W4ZMWB3S4", "status": "invalidated" }
```

Invalidated memories don't appear in the list by default. Add `include_invalidated=true` to see them:

```bash
curl -fsS 'http://127.0.0.1:8420/api/v1/memory?namespace=assistant&include_invalidated=true'
```

## Sessions {#sessions}

A memory session groups a set of conversation turns. `namespace` is required when you create one:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"namespace": "assistant", "user_id": "user_001"}' \
  http://127.0.0.1:8420/api/v1/memory/sessions
```

```json
{
  "created_at": "2026-10-03T23:25:59.267Z",
  "namespace": "assistant",
  "session_id": "90fbdf71-f7dd-4a7a-a46b-5eac344871f2",
  "title": "",
  "user_id": "user_001"
}
```

To write a turn to a session, use `POST /api/v1/memory/sessions/{session_id}/interactions`. `namespace`, `query_text`, and `response_text` are required.

## In development {#not-available}

| Capability | Status |
|---|---|
| Automatic memory extraction from conversations | In development. Not supported yet |
| Automatic handling of contradicting memories | In development. Not supported yet |

## Known limitation {#search-note}

`POST /api/v1/memory/search` requires `query`, `namespace`, and `user_id`. In testing, it didn't return memories written explicitly through `POST /api/v1/memory`, and a regular query against the same namespace didn't return them either. To read explicit memories, use the list endpoint above.

## Next steps {#next-steps}

- [Query and rerank](/docs/guides/query/)
- [Compatibility and status](/docs/resources/compatibility/)
- [HTTP API reference](/docs/reference/api/)
