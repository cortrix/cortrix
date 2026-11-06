# Deleting and Restoring Documents

Deleting a document is a soft delete. A deleted document can be restored until its retention window ends; after that the server removes it for good.

For a runnable walkthrough, see [examples/delete-restore](../examples/delete-restore/README.md).

## Delete

`DELETE /api/v1/documents/{id}?namespace=<namespace>` needs WRITE permission and returns `204` with no body. In the Python SDK this is `client.documents.delete(namespace, document_id)`.

Right after the call, the document no longer appears in document listings or query results, and reading it by id returns `404`. Its blocks, vectors and original file stay on disk until a cleanup removes them.

## Restore

`POST /api/v1/gc/restore` with `{"document_ids": ["..."]}` needs ADMIN permission. In the Python SDK this is `client.ops.gc.restore([...])`.

The response reports each id separately: each entry in `succeeded` carries a restored `id`, and each entry in `failed` carries an `error_code` and a `message`.

| `error_code` | Why | What to do |
|---|---|---|
| `CX_ERR_GC_RESTORE_EXPIRED` | The document was deleted longer ago than `gc.soft_delete_retention_days`. | Add the document again. |
| `CX_ERR_GC_RESTORE_SUPERSEDED` | Another version of the same uploaded file or watched file is live. `structured_data.live_doc_id` and `structured_data.namespace` name it. | Delete that version, then restore again. |
| `CX_ERR_GC_RESTORE_INCOMPLETE` | The document was deleted before it finished processing. | Add the document again. |
| `CX_ERR_GC_RESTORE_CHECK_FAILED` | The check could not run. | Retry the same request. |
| `CX_ERR_GC_DOC_NOT_FOUND` | No deleted document has this id: it was never deleted, it was already removed for good, or it was imported from a watched directory. | Add the document again, or restore the file in the watched folder. |

With authentication on, a read-write key gets `403` from `/gc/restore`. Hand the document ids to whoever holds an admin key.

## When a deleted document is removed for good

- `gc.soft_delete_retention_days` (default `30`) is how long a deleted document can be restored.
- A background cleanup runs every `gc.scan_interval_hours` (default `24`) and permanently removes documents past the window. `gc.enabled: false` turns the background cleanup off; the endpoints below still work.
- `POST /api/v1/gc/run` runs the cleanup now. `POST /api/v1/gc/purge` permanently removes soft-deleted documents without waiting for the window, and can't be undone. Both need ADMIN permission and the header `X-Ops-Confirm: true`.
- `GET /api/v1/gc/status` needs ADMIN permission and reports `soft_deleted_count`.

## Documents imported from watched directories

Documents imported by a directory watcher follow their source files. Deleting a file from the watched folder deletes its document outright, without the retention window; to get it back, restore the file in the folder. Removing the whole watcher (`DELETE /api/v1/watch/{id}` or `DELETE /api/v1/connector/watchers/{id}`) permanently deletes every document it imported, and `/gc/restore` can't bring them back. To stop syncing one namespace and keep its documents, unsubscribe that namespace with `DELETE /api/v1/connector/watchers/{id}/namespaces/{ns}`, which needs ADMIN permission.

## Keys for deleting and restoring

Every key created with `POST /api/v1/auth/api-keys` has ADMIN permission, so it can both delete and restore. To give an agent a key that can delete but not restore, configure a read-write key (`permissions: 3`) as an `auth.api_keys` entry; see [Compatibility](compatibility.md#auth-and-security-boundaries).
