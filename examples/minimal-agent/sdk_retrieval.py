#!/usr/bin/env python3
"""The retrieval step of an agent, written against the Cortrix Python SDK.

It loads three small notes into a namespace, asks one question across that
namespace and one that doesn't exist, and shows how to turn the response into
context the agent can use plus an honest note about what could not be searched.

Usage:
    pip install cortrix
    python sdk_retrieval.py [--base-url http://127.0.0.1:8420] [--keep]
"""

from __future__ import annotations

import argparse
import sys
import time
import uuid
from pathlib import Path

# TimeoutError is aliased: the bare name would shadow the builtin in this module.
from cortrix import Cortrix, CortrixError, TimeoutError as CortrixTimeoutError

NOTES = Path(__file__).parent / "notes"
QUESTION = "How long does a customer have to request a refund?"


def load(client: Cortrix, namespace: str, path: Path, timeout: float = 60.0) -> str:
    """Upload one file and wait until Cortrix has finished processing it."""
    task = client.documents.upload(namespace, path)
    deadline = time.monotonic() + timeout
    while task.status not in ("completed", "failed", "cancelled"):
        if time.monotonic() >= deadline:
            raise CortrixTimeoutError(
                f"{path.name} was still {task.status} after {timeout:.0f} s",
                category="timeout", retryable=True)
        time.sleep(0.5)
        task = client.documents.task_progress(task.task_id)
    return task.status


def retrieve(client: Cortrix, namespaces: list[str], question: str) -> dict:
    """Query Cortrix and sort the outcome into what an agent needs to decide its next step."""
    response = client.search(namespaces, question, top_k=3)
    retry, missing = [], []
    for failed in response.meta.namespaces_failed or []:
        if failed["retryable"]:
            # Retry only this namespace, after the wait Cortrix suggests.
            retry.append((failed["namespace"], failed["retry_after_ms"] or 1000))
        else:
            # Don't retry. Tell the user this part was not searched.
            missing.append((failed["namespace"], failed["category"]))
    return {
        "complete": response.meta.coverage_ratio >= 1,
        "coverage_ratio": response.meta.coverage_ratio,
        "context": [item.content.strip() for item in response.results],
        "retry": retry,
        "missing": missing,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default="http://127.0.0.1:8420")
    parser.add_argument("--keep", action="store_true", help="keep the example namespace when done")
    args = parser.parse_args()

    namespace = f"example-notes-{uuid.uuid4().hex[:8]}"
    client = Cortrix(base_url=args.base_url)
    try:
        client.namespaces.create(namespace, display_name="Minimal agent example")
        for note in sorted(NOTES.glob("*.md")):
            print(f"loaded {note.name}: {load(client, namespace, note)}")

        outcome = retrieve(client, [namespace, "not-a-real-namespace"], QUESTION)

        print(f"\nquestion: {QUESTION}")
        print(f"coverage_ratio: {outcome['coverage_ratio']}")
        for index, passage in enumerate(outcome["context"], start=1):
            print(f"context {index}: {passage.splitlines()[-1]}")
        for name, wait_ms in outcome["retry"]:
            print(f"retry later: {name} (after {wait_ms} ms)")
        for name, category in outcome["missing"]:
            print(f"not searched: {name} ({category})")
        if not outcome["complete"]:
            print("The agent should answer from the context above"
                  " and say which part it could not search.")
        return 0
    except CortrixError as error:
        print(f"Cortrix error: {error} "
              f"(category={error.category}, retryable={error.retryable})", file=sys.stderr)
        return 1
    finally:
        if not args.keep:
            try:
                client.namespaces.delete(namespace)
            except CortrixError:
                pass
        client.close()


if __name__ == "__main__":
    sys.exit(main())
