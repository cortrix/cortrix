---
title: Upload documents
description: Write documents into a namespace and track their processing.
weight: 10
---

On this page you'll create a namespace, upload documents in two different ways, confirm they've been processed, and learn how to delete one.

> [!NOTE]
> The output on this page is real output from `v1.0.0-rc.2`. It differs from the OpenAPI spec in several places: upload results use the statuses `pending` and `skipped` where the spec lists `queued`, `updating`, and `unchanged`; a finished task reports `completed` where the spec says `ready`; `source_type` can be `file`, which isn't in the spec's list; timestamps are epoch milliseconds; and the validation error shown at the end has a `code`, `request_id`, and `timestamp` that don't match the spec's patterns. Write client code against what the server returns, and recheck after an upgrade.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- `curl`.

The examples target the service the quickstart starts, which has no authentication. When you connect to a service with authentication turned on, send the `X-API-Key` header with each request.

## Two ways to upload {#two-upload-paths}

| | File upload | Inline upload |
|---|---|---|
| Endpoint | `POST /api/v1/namespaces/{ns}/documents` | `POST /api/v1/documents` |
| Request body | `multipart/form-data` | JSON |
| Best for | Uploading a file, including binary files | Text your program already has in memory |
| Returns | A `doc_id` right away | A `task_id`; processing is asynchronous |
| If the namespace doesn't exist | 404 right away | The request is accepted and the task fails later |

Either way, the content goes through the same pipeline: parsing, chunking, embedding, and indexing.

## Steps {#steps}

{{% steps %}}

### Create a namespace {#create-namespace}

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"name": "guide", "display_name": "Guide"}' \
  http://127.0.0.1:8420/api/v1/namespaces
```

```json
{
  "block_count": 0,
  "created_at": 1791069436679,
  "doc_count": 0,
  "name": "guide",
  "namespace": "guide",
  "status": "active",
  "updated_at": 1791069436679
}
```

### Upload a file {#upload-a-file}

First create a text file:

```bash
printf 'Refund policy\n\nCustomers can request a refund within 30 days of purchase.\nRefunds are issued to the original payment method within 5 business days.\n' \
  > refund-policy.txt
```

Upload it as multipart. `file` is required. `title` and `metadata` (a JSON string) are optional:

```bash
curl -fsS \
  -F 'file=@refund-policy.txt' \
  -F 'title=Refund policy' \
  http://127.0.0.1:8420/api/v1/namespaces/guide/documents
```

```json
{
  "content_hash": "4cae8f4fef140499f28815e2372a3027b5dbb2634e14e7766bf609011f56545a",
  "doc_id": "01M420XEVFXR2YE9NZ6E49X8M7",
  "message": "Document uploaded successfully, processing queued",
  "source_path": "refund-policy.txt",
  "status": "pending"
}
```

Uploading a file with the same content again doesn't reprocess it. Cortrix detects the duplicate by content hash and returns HTTP 200 with the same `doc_id`:

```json
{
  "content_hash": "4cae8f4fef140499f28815e2372a3027b5dbb2634e14e7766bf609011f56545a",
  "doc_id": "01M420XEVFXR2YE9NZ6E49X8M7",
  "message": "Document unchanged, skipping reprocessing",
  "source_path": "refund-policy.txt",
  "status": "skipped"
}
```

### Upload inline content {#upload-inline-content}

`namespace` and `content` are required. `filename` and `metadata` are optional:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespace": "guide",
    "content": "Shipping policy. Orders ship within 2 business days. Express shipping is available at checkout.",
    "filename": "shipping-policy.txt"
  }' \
  http://127.0.0.1:8420/api/v1/documents
```

The service returns HTTP 202 and a task:

```json
{
  "namespace": "guide",
  "status": "queued",
  "task_id": "01M420XEVZMH2RRDH2CCZ4R8BE"
}
```

### Check task progress {#check-progress}

Look up the task with the `task_id` from the previous step:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/documents/tasks/01M420XEVZMH2RRDH2CCZ4R8BE/progress
```

```json
{
  "doc_id": "01M420XEVZMH2RRDH2CCZ4R8BC",
  "error_code": null,
  "error_msg": null,
  "meta": {
    "agent_decision_hint": "done_fetch_doc",
    "current_phase": "parsing",
    "retryable_if_fail": true
  },
  "processed_pages": 1,
  "progress_pct": 100.0,
  "status": "completed",
  "task_id": "01M420XEVZMH2RRDH2CCZ4R8BE",
  "total_pages": 1
}
```

A `status` of `completed` with `error_code` set to `null` means processing succeeded. `meta.agent_decision_hint` tells an agent what to do next.

> [!WARNING]
> An inline upload returns 202 even when the namespace doesn't exist. The error shows up only in the task progress: `error_code` is `CX_ERR_SPC_PROCESS_FAILED` and `meta.agent_decision_hint` is `inspect_error_code`. So always check task progress after an inline upload. Don't rely on the upload request succeeding.

### Confirm the documents are ready {#list-documents}

```bash
curl -fsS 'http://127.0.0.1:8420/api/v1/documents?namespace=guide'
```

```json
{
  "documents": [
    {
      "document_id": "01M420XEVZMH2RRDH2CCZ4R8BC",
      "filename": "shipping-policy.txt",
      "namespace": "guide",
      "source_type": "file",
      "status": "ready"
    },
    {
      "document_id": "01M420XEVFXR2YE9NZ6E49X8M7",
      "filename": "refund-policy.txt",
      "namespace": "guide",
      "source_type": "http_upload",
      "status": "ready"
    }
  ],
  "total": 2
}
```

Queries can find a document once its `status` is `ready`. The list supports paging with `limit` (default 50, maximum 200) and `offset`.

{{% /steps %}}

## Get a single document {#get-a-document}

Documents are stored per namespace, so pass `namespace` when you fetch one:

```bash
curl -fsS 'http://127.0.0.1:8420/api/v1/documents/01M420XEVFXR2YE9NZ6E49X8M7?namespace=guide'
```

## Delete a document {#delete-a-document}

```bash
curl -fsS -X DELETE \
  'http://127.0.0.1:8420/api/v1/documents/01M420XEVZMH2RRDH2CCZ4R8BC?namespace=guide'
```

A successful delete returns HTTP 204 with no body. The document and all of its blocks disappear from the list and from query results.

This is a soft delete: the document can still be restored during a retention period set by `gc.soft_delete_retention_days`. The default is 30 days, and 0 means delete immediately. After the retention period, garbage collection removes it permanently.

## Batch submit {#batch-submit}

`POST /api/v1/documents/batch` submits 1 to 100 documents to one namespace in a single request. Each accepted document becomes an asynchronous task.

This endpoint uses partial-success semantics: it returns HTTP 200 even if some documents fail. Accepted documents are in `results[]`, failures are in `meta.failed[]`, and every failure carries structured error fields.

For the full request and response shapes, see the [HTTP API reference](/docs/reference/api/).

## When something goes wrong {#errors}

Error responses share one structure, so you can decide whether to retry from `retryable` and `category`. For example, creating a namespace without `name`:

```json
{
  "error": {
    "category": "permanent",
    "code": "INVALID_ARGUMENT",
    "message": "Missing 'name' field",
    "request_id": "571fa754f07d1413ef27cf5af3cb0367",
    "retry_after_ms": null,
    "retryable": false,
    "structured_data": {},
    "timestamp": "2026-10-03T23:17:05.349Z"
  }
}
```

## Next steps {#next-steps}

- [Query and rerank](/docs/guides/query/): query the documents you just uploaded.
- [Python SDK](/docs/integrations/python-sdk/): do the same thing with `client.documents.upload()`.
- [HTTP API reference](/docs/reference/api/)
