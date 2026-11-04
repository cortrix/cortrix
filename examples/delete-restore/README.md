# Delete and restore

Deleting a document in Cortrix is a soft delete. The document disappears from queries, document lists, and reads by id at once, but its blocks, vectors, and original file stay on disk for 30 days (`gc.soft_delete_retention_days`). Inside that window, `POST /api/v1/gc/restore` brings it back by id, with no reprocessing. After a purge, or after the daily cleanup once the window has passed, the document can no longer be restored. A manual purge has to be confirmed with the `X-Ops-Confirm: true` header.

This example has two scripts:

- `delete_restore.py` deletes the wrong document from a small support knowledge base, watches the query answer go wrong, and restores the document by id. With `--purge` it also shows a purge, after which the document can't be restored.
- `new_version.py` uploads a file again under the same name and shows what happens when you try to restore the version it replaced.

Neither script needs an LLM key.

## Requirements

- A Cortrix server built from `main`. Follow the [Quickstart](https://cortrix.ai/docs/get-started/quickstart/), but clone `main` instead of the release tag:

```bash
git clone --depth 1 https://github.com/cortrix/cortrix.git
cd cortrix
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

- Python 3.10 or later, in a virtual environment, with the SDK from the same checkout:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install ./sdk/python
```

Both scripts target `http://127.0.0.1:8420`. Pass `--base-url` to use another address. If your server has authentication on, pass `--api-key` or set `CORTRIX_API_KEY`; restoring needs a key with ADMIN permission. Each run creates a namespace with a new name and deletes it at the end; pass `--keep` to leave it in place. A deleted namespace name can't be created again on the same server, which is why the names carry a timestamp.

## Delete the wrong document: `delete_restore.py`

From the repository root:

```bash
python examples/delete-restore/delete_restore.py
```

What it does:

1. Loads the five Markdown files in `docs/` into a new namespace: the current refund policy, the retired 2024 refund policy, and three unrelated support articles.
2. Lists the documents and prints each id. Restoring goes by id, and a deleted document no longer shows up in the list, so an agent should record the id before it deletes.
3. Asks "How long does a refund take?" The current policy ranks first.
4. Deletes the current refund policy. The `DELETE` returns 204, and the same question now gets the retired policy's answer first.
5. Restores two ids in one call: the deleted document's id, and the same id with its last character changed, the way a copied id can go wrong. The response reports each id on its own and gives `coverage_ratio`; see [`../partial-success`](../partial-success) for the same contract on queries.
6. Asks the question again. The current policy is back at the top with the score it had before the delete.

Output from a build of `main` at `0d8d2b8`, CPU only, no LLM:

```text
namespace: delete-restore-1793762497
documents:
  01M6J974Z0EDVXW3J95CM1VT80  invoices.md
  01M6J975ZW2CKGDN031MNWNEYA  loyalty-points.md
  01M6J976H5RJRFXAJCQW3V53VT  refund-policy-2024.md
  01M6J9772F2TC8PWWWZ8RR18Y1  refund-policy.md
  01M6J977KREHJG0A34FDV4J8ST  shipping.md
before the delete:
  1  refund-policy.md         rerank_score=0.9866  How long a refund takes: the money goes back to ...
  2  refund-policy-2024.md    rerank_score=0.7770  The money goes back to the original payment ...
  3  invoices.md              rerank_score=0.0110  You can request an electronic invoice from the ...
DELETE /documents/01M6J9772F2TC8PWWWZ8RR18Y1 -> 204
after the delete:
  1  refund-policy-2024.md    rerank_score=0.7770  The money goes back to the original payment ...
  2  invoices.md              rerank_score=0.0110  You can request an electronic invoice from the ...
  3  shipping.md              rerank_score=0.0010  Orders within the same city arrive the same ...
restore 01M6J9772F2TC8PWWWZ8RR18Y1 and 01M6J9772F2TC8PWWWZ8RR18YX:
  succeeded  01M6J9772F2TC8PWWWZ8RR18Y1
  failed     01M6J9772F2TC8PWWWZ8RR18YX  CX_ERR_GC_DOC_NOT_FOUND  retryable=false
             no deleted document with this id
  coverage_ratio=0.5
after the restore:
  1  refund-policy.md         rerank_score=0.9866  How long a refund takes: the money goes back to ...
  2  refund-policy-2024.md    rerank_score=0.7770  The money goes back to the original payment ...
  3  invoices.md              rerank_score=0.0110  You can request an electronic invoice from the ...
```

The ids are new on every run. The scores depend on the models in your build.

### Purge: `--purge`

> `POST /api/v1/gc/purge` removes every soft-deleted document on the server, in every namespace, and can't be undone. Run `--purge` only against a server where nothing else is waiting to be restored.

```bash
python examples/delete-restore/delete_restore.py --purge
```

After the steps above, the script deletes the retired 2024 policy on purpose, calls the purge once without the confirmation header and once with it, and then tries to restore the purged document:

```text
DELETE /documents/01M6J976H5RJRFXAJCQW3V53VT (refund-policy-2024.md) -> 204
POST /gc/purge without X-Ops-Confirm -> 400 CX_ERR_OPS_CONFIRM_REQUIRED
POST /gc/purge with X-Ops-Confirm: true -> 202
restore 01M6J976H5RJRFXAJCQW3V53VT:
  failed     01M6J976H5RJRFXAJCQW3V53VT  CX_ERR_GC_DOC_NOT_FOUND  retryable=false
             no deleted document with this id
  coverage_ratio=0.0
```

`POST /api/v1/gc/run` also needs the header. It removes only the documents whose 30 days are up, which is what the server does on its own once a day.

## Upload a new version: `new_version.py`

```bash
python examples/delete-restore/new_version.py
```

A file uploaded through `POST /api/v1/namespaces/{name}/documents` (the Web UI uses this route) is versioned by its file name. Uploading the same name again creates a new version; the response names the version it replaces in `superseded_doc_id`, and that old version is soft-deleted once the new one is ready. Restoring the old version while the new one is live is refused, and the message says what to do:

```text
namespace: new-version-1793762508
upload refund-policy.md: doc_id=01M6J97G0Q56CJKB2HNR03DXKK
upload refund-policy.md again: doc_id=01M6J97GKED58ZS15BANHEGVMT superseded_doc_id=01M6J97G0Q56CJKB2HNR03DXKK
restore the first version 01M6J97G0Q56CJKB2HNR03DXKK:
  CX_ERR_GC_RESTORE_SUPERSEDED
  another version is live: delete 01M6J97GKED58ZS15BANHEGVMT in namespace new-version-1793762508, then restore 01M6J97G0Q56CJKB2HNR03DXKK
DELETE /documents/01M6J97GKED58ZS15BANHEGVMT -> 204
restore 01M6J97G0Q56CJKB2HNR03DXKK again: succeeded=['01M6J97G0Q56CJKB2HNR03DXKK'] coverage_ratio=1.0
```

The refusal also carries the live version's id and namespace in `structured_data` (`live_doc_id`, `namespace`), so an agent can follow it without parsing the message.

## What to keep in mind

- Restoring needs ADMIN permission; deleting needs only WRITE. With authentication on, a read-write key gets `403 ADMIN permission required` from `/gc/restore`, so hand the ids to whoever holds an admin key.
- Documents imported by a directory watcher follow their source files. Deleting the file from the watched folder, or removing the watcher with its documents, deletes those documents outright, without the 30-day window. To get one back, restore the file in the folder.
- A document deleted before it finished processing can't be restored: `/gc/restore` answers `CX_ERR_GC_RESTORE_INCOMPLETE`. Upload the file again.
- On `v1.0.0-rc.2`, a deleted document can still come back from vector search until it is purged (#169). That's fixed on `main`, which is why this example asks for a build of `main`.
