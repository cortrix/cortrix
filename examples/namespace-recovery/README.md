# Namespace recovery check

Each Cortrix namespace keeps its own vector index, SQLite store, and write journal. A document's write to that namespace either completes everywhere or leaves nothing behind, and a restart checks and cleans up each namespace before serving it.

This example has two scripts:

- `crash_check.py` kills the server in the middle of a batch write, starts it again, and checks that every document ends up whole.
- `two_projects.py` keeps two projects in their own namespaces and runs one query across both.

Neither script needs an LLM key.

## Requirements

- A running Quick Start. See the [Quickstart](https://cortrix.ai/docs/get-started/quickstart/).
- Python 3.10 or later, in a virtual environment, with the SDK installed:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install cortrix
```

- For `crash_check.py`: Docker, and permission to restart the Cortrix container.

Both scripts target `http://127.0.0.1:8420`. Pass `--base-url` to use another address. Each run creates namespaces with new names and deletes them at the end; pass `--keep` to leave them in place. A deleted namespace name can't be created again on the same server, which is why the names carry a timestamp.

## Kill the server mid-write: `crash_check.py`

> This sends SIGKILL to your Cortrix container and starts it again. Run it against a Quick Start you don't mind restarting.

From the repository root:

```bash
python examples/namespace-recovery/crash_check.py
```

By default it restarts the Quick Start's `cortrix` service with `docker compose -f deploy/docker-compose.yml`. If your container runs some other way, pass its name with `--container`.

What it does:

1. Creates a namespace and submits 40 Markdown documents of about 24 KB through the SDK.
2. Watches the upload tasks. As soon as at least one document is done and another is still being written, it sends SIGKILL to the server and notes which documents were caught mid-write. The server gets no chance to clean up.
3. Starts the server on the same data and waits until it is healthy. While the namespace loads, Cortrix checks every write that never committed: a write that reached every store is kept, a partial one is removed, and documents that were mid-write go back into the queue.
4. Waits for all 40 tasks to finish and checks through the public API that all 40 documents are ready and that the namespace holds exactly the blocks its documents account for.
5. Uploads the documents that were caught mid-write into a second, clean namespace and compares block counts document by document. A document that kept part of the interrupted write would show up with extra blocks.

The public API reports block counts, not vector counts, so the script checks the document and block side. The server log shows the recovery itself, for example `Recovered 2 crashed documents (2 processing, 0 deleting)`.

Output from a build of `main` at `fced345`, CPU only, no LLM:

```text
namespace: recovery-check-1792578931
submitted 40 documents
killed the server with SIGKILL: completed 2, processing 2, queued 36
caught mid-write: doc-002.md, doc-003.md
healthy again after 15 s
tasks after restart: completed 40
documents ready: 40/40
blocks in namespace: 2440 (sum over documents: 2440)
doc-002.md: 61 blocks after recovery, 61 in a clean upload
doc-003.md: 61 blocks after recovery, 61 in a clean upload
PASS
```

Which documents get caught mid-write, how long the server takes to come back, and how long the batch takes depend on your machine. On a CPU-only machine with two workers, the 40 documents take around 10 minutes.

Documents submitted through the SDK or MCP go back into the queue on restart. In `v1.0.0-rc.2`, documents uploaded as files through `POST /api/v1/namespaces/{name}/documents` (the Web UI uses this route) that were still queued at the restart were not resubmitted; that is fixed on `main` (#217).

## Query across projects: `two_projects.py`

```bash
python examples/namespace-recovery/two_projects.py
```

It creates two namespaces, one per project, loads each project's own document from `docs/`, and loads the shared `coding-standards.md` into both. Then it asks one question across the two namespaces.

Output from the same build:

```text
proj-pay-1792579711      coding-standards.md    rerank_score=0.9949
proj-search-1792579711   search-api.md          rerank_score=0.0004
proj-pay-1792579711      payment-gateway.md     rerank_score=0.0001
coverage_ratio=1.0 deduplicated_chunks=1
```

What to look for:

- `coding-standards.md` is stored in both namespaces but comes back once. `deduplicated_chunks=1` counts the copy that was merged away.
- `coverage_ratio=1.0` means both namespaces answered. When one can't be queried, the request still succeeds and says which part is missing; see [`../partial-success`](../partial-success).
- The merged order follows each result's score inside its own namespace. Without an LLM configured that score comes from the result's rank, so each namespace's best match sits near the top even when it isn't relevant. To compare results across namespaces, use `rerank_score`, for example by dropping results below a threshold before an agent answers from them.
