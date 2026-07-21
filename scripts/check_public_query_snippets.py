#!/usr/bin/env python3
"""Reject public Markdown query examples that use the deprecated singular key."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SINGULAR_NAMESPACE = re.compile(r'"namespace"\s*:')
QUERY_PAYLOAD = re.compile(r'"query"\s*:')


def fenced_blocks(text: str) -> list[tuple[int, str]]:
    blocks: list[tuple[int, str]] = []
    start_line = 0
    lines: list[str] = []
    in_block = False
    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.lstrip().startswith("```"):
            if in_block:
                blocks.append((start_line, "\n".join(lines)))
                lines = []
                in_block = False
            else:
                start_line = line_number + 1
                in_block = True
            continue
        if in_block:
            lines.append(line)
    return blocks


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    paths = [root / "README.md"]
    paths.extend(sorted((root / "docs").rglob("*.md")))
    paths.extend(sorted((root / "examples").rglob("*.md")))
    failures: list[str] = []
    checked = 0
    for path in paths:
        if not path.is_file():
            continue
        for line_number, block in fenced_blocks(path.read_text(encoding="utf-8")):
            if QUERY_PAYLOAD.search(block) and SINGULAR_NAMESPACE.search(block):
                failures.append(f"{path.relative_to(root)}:{line_number}")
            if QUERY_PAYLOAD.search(block):
                checked += 1
    if failures:
        print("PUBLIC_QUERY_SNIPPET_CONTRACT=FAIL")
        for failure in failures:
            print(f"deprecated_singular_namespace={failure}")
        return 1
    print("PUBLIC_QUERY_SNIPPET_CONTRACT=PASS")
    print(f"query_blocks_checked={checked}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
