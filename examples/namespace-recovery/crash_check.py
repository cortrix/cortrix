#!/usr/bin/env python3
"""Kill Cortrix while it is writing a namespace, restart it, and check the result.

The script creates a namespace with a unique name, submits a batch of documents
through the Python SDK, and sends SIGKILL to the server once some documents are
done and others are still being written. It then starts the server again on the
same data, waits for the interrupted documents to be processed again, and checks
through the public API that every document is ready and that the documents
caught mid-write have exactly the blocks a clean upload of them produces.

This restarts your Cortrix container. Run it against a Quick Start you don't
mind restarting.
"""

from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

from cortrix import Cortrix

WORDS = ("namespace vector index sqlite blob write coordinator pending commit "
         "rollback recovery crash block chunk embedding query merge agent project "
         "archive reimport scope tenant memory snapshot journal").split()
SECTIONS = 60
WORDS_PER_SECTION = 60
TERMINAL = ("completed", "failed", "cancelled")


def make_docs(directory: Path, count: int) -> list[Path]:
    """Write documents of about 24 KB, each split into many sections."""
    paths = []
    for i in range(count):
        rnd = random.Random(i)
        parts = [f"# Document {i:03d}\n"]
        for s in range(SECTIONS):
            body = " ".join(rnd.choice(WORDS) for _ in range(WORDS_PER_SECTION))
            parts.append(f"## Section {s}\n\nMarker doc{i:03d}-sec{s:02d}. {body}.\n")
        path = directory / f"doc-{i:03d}.md"
        path.write_text("\n".join(parts))
        paths.append(path)
    return paths


def get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=10) as resp:
        return json.load(resp)


def wait_healthy(base_url: str, timeout: float) -> float:
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        try:
            get_json(f"{base_url}/api/v1/health")
            return time.monotonic() - start
        except OSError:
            time.sleep(0.5)
    raise SystemExit(f"Cortrix did not come back within {timeout:.0f} s")


def statuses(client: Cortrix, tasks: dict[str, Path]) -> dict[str, list[Path]]:
    by_status: dict[str, list[Path]] = {}
    for task_id, path in tasks.items():
        status = client.documents.task_progress(task_id).status
        by_status.setdefault(status, []).append(path)
    return by_status


def summary(by_status: dict[str, list[Path]]) -> str:
    return ", ".join(f"{status} {len(paths)}" for status, paths in sorted(by_status.items()))


def wait_done(client: Cortrix, tasks: dict[str, Path], timeout: float) -> dict[str, list[Path]]:
    deadline = time.monotonic() + timeout
    while True:
        by_status = statuses(client, tasks)
        if sum(len(by_status.get(s, [])) for s in TERMINAL) == len(tasks):
            return by_status
        if time.monotonic() > deadline:
            raise SystemExit(f"FAIL: still in progress after {timeout:.0f} s: {summary(by_status)}")
        time.sleep(2)


def block_counts(base_url: str, namespace: str) -> dict[str, tuple[str, int]]:
    listed = get_json(f"{base_url}/api/v1/namespaces/{namespace}/documents?limit=1000")
    return {d["source_path"]: (d["status"], d["block_count"]) for d in listed["documents"]}


def docker(args: argparse.Namespace, action: list[str]) -> None:
    if args.container:
        cmd = ["docker", *action, args.container]
    else:
        cmd = ["docker", "compose", "-f", args.compose_file, *action, "cortrix"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default="http://127.0.0.1:8420")
    parser.add_argument("--docs", type=int, default=40, help="documents to submit (default 40)")
    parser.add_argument("--container", help="container name; default is the Quick Start compose service")
    parser.add_argument("--compose-file", default="deploy/docker-compose.yml")
    parser.add_argument("--keep", action="store_true", help="keep the namespace afterwards")
    args = parser.parse_args()

    client = Cortrix(base_url=args.base_url)
    namespace = f"recovery-check-{int(time.time())}"
    control = f"{namespace}-control"
    client.namespaces.create(namespace)
    print(f"namespace: {namespace}")

    with tempfile.TemporaryDirectory() as tmp:
        paths = make_docs(Path(tmp), args.docs)
        tasks = {client.documents.upload(namespace, p).task_id: p for p in paths}
        print(f"submitted {len(tasks)} documents")

        # Kill once some documents are done and the next ones are mid-write.
        while True:
            by_status = statuses(client, tasks)
            if by_status.get("completed") and by_status.get("processing"):
                break
            if sum(len(by_status.get(s, [])) for s in TERMINAL) == len(tasks):
                print("every document finished before the kill; rerun with more --docs")
                return 1
        docker(args, ["kill", "-s", "KILL"])
        interrupted = sorted(by_status["processing"])
        print(f"killed the server with SIGKILL: {summary(by_status)}")
        print(f"caught mid-write: {', '.join(p.name for p in interrupted)}")

        docker(args, ["start"])
        print(f"healthy again after {wait_healthy(args.base_url, 300):.0f} s")
        print(f"tasks after restart: {summary(wait_done(client, tasks, 1800))}")

        # A clean upload of the interrupted documents, for comparison.
        client.namespaces.create(control)
        wait_done(client, {client.documents.upload(control, p).task_id: p for p in interrupted}, 600)

    docs = block_counts(args.base_url, namespace)
    clean = block_counts(args.base_url, control)
    ready = sum(status == "ready" for status, _ in docs.values())
    total = sum(blocks for _, blocks in docs.values())
    stats = get_json(f"{args.base_url}/api/v1/system/namespaces/{namespace}/stats")
    print(f"documents ready: {ready}/{args.docs}")
    print(f"blocks in namespace: {stats['chunk_count']} (sum over documents: {total})")
    same = True
    for p in interrupted:
        after, expected = docs[p.name][1], clean[p.name][1]
        same &= after == expected
        print(f"{p.name}: {after} blocks after recovery, {expected} in a clean upload")

    # Leftovers of the interrupted write would show up as extra blocks.
    ok = ready == args.docs and stats["chunk_count"] == total and same
    print("PASS" if ok else "FAIL")

    if not args.keep:
        for ns in (namespace, control):
            client.namespaces.delete(ns)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
