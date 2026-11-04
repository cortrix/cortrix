#!/usr/bin/env python3
"""Delete the wrong document, then restore it by id.

A small support knowledge base holds the current refund policy and a retired
2024 version. The current one gets deleted by mistake: queries now answer from
the retired policy. Deleting is a soft delete, so restoring the document by its
id brings it back with the same score it had before, without reprocessing.

With --purge the script also shows a permanent delete: it deletes the retired
policy on purpose, purges, and checks that the purged document can't be
restored.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

import httpx
from cortrix import Cortrix

DOCS = Path(__file__).parent / "docs"
QUESTION = "How long does a refund take?"


def load(client: Cortrix, namespace: str, path: Path, timeout: float = 120.0) -> None:
    """Upload one file and wait until Cortrix has finished processing it."""
    task = client.documents.upload(namespace, path)
    deadline = time.monotonic() + timeout
    while task.status not in ("completed", "failed", "cancelled"):
        if time.monotonic() >= deadline:
            raise SystemExit(f"{path.name} was still {task.status} after {timeout:.0f} s")
        time.sleep(0.5)
        task = client.documents.task_progress(task.task_id)
    if task.status != "completed":
        raise SystemExit(f"{path.name} ended as {task.status}")


def show(client: Cortrix, namespace: str, question: str, label: str) -> None:
    print(f"{label}:")
    resp = client.search(namespace, question, top_k=3)
    for i, r in enumerate(resp.results, 1):
        snippet = " ".join(r.content.split("\n", 2)[-1].split())
        if len(snippet) > 48:
            snippet = snippet[:48].rsplit(" ", 1)[0] + " ..." if " " in snippet[:48] else snippet[:48] + "..."
        print(f"  {i}  {r.metadata['source_path']:<24} rerank_score={r.rerank_score:.4f}  {snippet}")


def print_restore(result: dict) -> None:
    for ok in result.get("succeeded", []):
        print(f"  succeeded  {ok['id']}")
    for bad in result.get("failed", []):
        print(f"  failed     {bad['id']}  {bad['error_code']}  retryable={str(bad['retryable']).lower()}")
        print(f"             {bad['message']}")
    print(f"  coverage_ratio={result.get('coverage_ratio')}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default="http://127.0.0.1:8420")
    parser.add_argument("--api-key", default=os.environ.get("CORTRIX_API_KEY"),
                        help="needed only when auth is on; restoring takes an ADMIN key")
    parser.add_argument("--docs", type=Path, default=DOCS, help="folder of Markdown files to load")
    parser.add_argument("--question", default=QUESTION)
    parser.add_argument("--current", default="refund-policy.md",
                        help="the document that gets deleted by mistake")
    parser.add_argument("--retired", default="refund-policy-2024.md",
                        help="the document --purge deletes for good")
    parser.add_argument("--purge", action="store_true",
                        help="also purge; this permanently removes EVERY soft-deleted document on the server")
    parser.add_argument("--keep", action="store_true", help="keep the namespace afterwards")
    args = parser.parse_args()

    client = Cortrix(base_url=args.base_url, api_key=args.api_key)
    headers = {"Authorization": f"Bearer {args.api_key}"} if args.api_key else {}
    api = httpx.Client(base_url=args.base_url.rstrip("/") + "/api/v1", headers=headers, timeout=60)

    # A deleted namespace name can't be created again, so every run uses a new name.
    namespace = f"delete-restore-{int(time.time())}"
    client.namespaces.create(namespace)
    print(f"namespace: {namespace}")
    for path in sorted(args.docs.glob("*.md")):
        load(client, namespace, path)

    # Record ids before deleting anything: a deleted document no longer shows up
    # in the list, and restoring goes by id.
    ids = {d.filename: d.document_id for d in client.documents.list(namespace).documents}
    print("documents:")
    for name, doc_id in sorted(ids.items(), key=lambda kv: kv[1]):
        print(f"  {doc_id}  {name}")
    current = ids[args.current]

    show(client, namespace, args.question, "before the delete")

    # Delete through the REST route, which takes the namespace as a query parameter.
    r = api.delete(f"/documents/{current}", params={"namespace": namespace})
    print(f"DELETE /documents/{current} -> {r.status_code}")
    show(client, namespace, args.question, "after the delete")

    # The second id is the same one with its last character changed, the way a
    # copied id can go wrong. Each id gets its own result.
    typo = current[:-1] + ("X" if current[-1] != "X" else "Y")
    print(f"restore {current} and {typo}:")
    print_restore(client.ops.gc.restore([current, typo]))
    show(client, namespace, args.question, "after the restore")

    if args.purge:
        retired = ids[args.retired]
        r = api.delete(f"/documents/{retired}", params={"namespace": namespace})
        print(f"DELETE /documents/{retired} ({args.retired}) -> {r.status_code}")
        r = api.post("/gc/purge")
        print(f"POST /gc/purge without X-Ops-Confirm -> {r.status_code} {r.json()['error']['code']}")
        r = api.post("/gc/purge", headers={"X-Ops-Confirm": "true"})
        print(f"POST /gc/purge with X-Ops-Confirm: true -> {r.status_code}")
        print(f"restore {retired}:")
        print_restore(client.ops.gc.restore([retired]))

    if not args.keep:
        client.namespaces.delete(namespace)
    return 0


if __name__ == "__main__":
    sys.exit(main())
