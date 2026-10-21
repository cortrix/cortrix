#!/usr/bin/env python3
"""Keep two projects in their own namespaces and query across both.

Each project gets a namespace and its own document. The shared coding
standards go into both. One query then spans the two namespaces: the shared
document comes back once, and `rerank_score` says how relevant each result is.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from cortrix import Cortrix

DOCS = Path(__file__).parent / "docs"
QUESTION = "What has to pass before code is committed?"


def load(client: Cortrix, namespace: str, path: Path, timeout: float = 60.0) -> str:
    """Upload one file and wait until Cortrix has finished processing it."""
    task = client.documents.upload(namespace, path)
    deadline = time.monotonic() + timeout
    while task.status not in ("completed", "failed", "cancelled"):
        if time.monotonic() >= deadline:
            raise SystemExit(f"{path.name} was still {task.status} after {timeout:.0f} s")
        time.sleep(0.5)
        task = client.documents.task_progress(task.task_id)
    return task.status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default="http://127.0.0.1:8420")
    parser.add_argument("--keep", action="store_true", help="keep the namespaces afterwards")
    args = parser.parse_args()

    client = Cortrix(base_url=args.base_url)
    # A deleted namespace name can't be created again, so every run uses new names.
    suffix = int(time.time())
    pay, search = f"proj-pay-{suffix}", f"proj-search-{suffix}"
    for ns in (pay, search):
        client.namespaces.create(ns)
    load(client, pay, DOCS / "payment-gateway.md")
    load(client, search, DOCS / "search-api.md")
    for ns in (pay, search):
        load(client, ns, DOCS / "coding-standards.md")

    resp = client.search([pay, search], QUESTION, top_k=3)
    for r in resp.results:
        print(f"{r.namespace:<24} {r.metadata['source_path']:<22} rerank_score={r.rerank_score:.4f}")
    print(f"coverage_ratio={resp.meta.coverage_ratio} "
          f"deduplicated_chunks={resp.meta.deduplicated_chunks_count}")

    if not args.keep:
        for ns in (pay, search):
            client.namespaces.delete(ns)
    return 0


if __name__ == "__main__":
    sys.exit(main())
