#!/usr/bin/env python3
"""Check the pinned cortrix_core source universe against CMakeLists.txt.

The nightly coverage gates (scripts/ci/coverage.sh and scripts/ci/llvmcov.sh)
fail closed when the set of cortrix_core translation units differs from the
count and manifest SHA-256 pinned at the top of each script. That pin only
runs in the nightly lane, so a PR that adds a core source file without
updating it merges green and every nightly run after it fails before any
coverage threshold is evaluated.

This check recomputes the same manifest (sorted repository-relative paths,
one per line) from the cortrix_core source list in CMakeLists.txt, without a
build, so the PR that changes the source list is the one asked to update the
pins.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path


SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")
SCRIPTS = ("scripts/ci/coverage.sh", "scripts/ci/llvmcov.sh")


def fail(message: str) -> None:
    print(f"core universe check FAILED: {message}", file=sys.stderr)
    sys.exit(1)


def core_sources(cmake_text: str) -> list[str]:
    # A source added to cortrix_core anywhere else would be in the nightly
    # manifest (built from compile_commands.json) but not in this one.
    if re.search(r"target_sources\s*\(\s*cortrix_core\b", cmake_text):
        fail("target_sources(cortrix_core ...) is not supported by this check; "
             "list core sources in add_library(cortrix_core STATIC ...)")
    match = re.search(r"add_library\(\s*cortrix_core\s+STATIC\b(.*?)\n\)",
                      cmake_text, re.S)
    if match is None:
        fail("add_library(cortrix_core STATIC ...) not found in CMakeLists.txt")
    sources = set()
    for line in match.group(1).splitlines():
        for token in line.split("#", 1)[0].split():
            if token.endswith(SOURCE_SUFFIXES):
                sources.add(token)
    if not sources:
        fail("no sources found in add_library(cortrix_core STATIC ...)")
    return sorted(sources)


def pinned(script_text: str, name: str, pattern: str, path: str) -> str:
    match = re.search(rf"^{name}={pattern}$", script_text, re.M)
    if match is None:
        fail(f"{path}: {name} not found")
    return match.group(1)


def zero_map_allowlist(script_text: str, path: str) -> set[str]:
    match = re.search(r"zero_map_allowlist = \{(.*?)\}", script_text, re.S)
    if match is None:
        fail(f"{path}: zero_map_allowlist not found")
    return set(re.findall(r'"([^"]+)"', match.group(1)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    root = parser.parse_args().root

    sources = core_sources((root / "CMakeLists.txt").read_text(encoding="utf-8"))
    manifest = "".join(f"{source}\n" for source in sources)
    count = len(sources)
    sha256 = hashlib.sha256(manifest.encode("utf-8")).hexdigest()

    problems = []
    mapped_expected = None
    for path in SCRIPTS:
        text = (root / path).read_text(encoding="utf-8")
        pinned_count = pinned(text, "CORE_TARGET_TU_EXPECTED", r"(\d+)", path)
        pinned_sha = pinned(text, "CORE_TARGET_SOURCE_SHA256_EXPECTED",
                            r'"([0-9a-f]{64})"', path)
        if int(pinned_count) != count:
            problems.append(f"{path}: CORE_TARGET_TU_EXPECTED={pinned_count}")
        if pinned_sha != sha256:
            problems.append(f"{path}: CORE_TARGET_SOURCE_SHA256_EXPECTED={pinned_sha}")
        if path.endswith("llvmcov.sh"):
            allowlist = zero_map_allowlist(text, path)
            missing = sorted(allowlist - set(sources))
            if missing:
                problems.append(f"{path}: zero_map_allowlist names non-core sources {missing}")
            mapped = pinned(text, "CORE_MAPPED_TU_EXPECTED", r"(\d+)", path)
            mapped_expected = count - len(allowlist)
            if int(mapped) != mapped_expected:
                problems.append(f"{path}: CORE_MAPPED_TU_EXPECTED={mapped}")

    if problems:
        fail("the cortrix_core source list changed; update the pins in "
             f"{' and '.join(SCRIPTS)}:\n  "
             + "\n  ".join(problems)
             + f"\n  current: CORE_TARGET_TU_EXPECTED={count} "
             f"CORE_MAPPED_TU_EXPECTED={mapped_expected} "
             f"CORE_TARGET_SOURCE_SHA256_EXPECTED=\"{sha256}\"")
    print(f"core universe pin OK: {count} sources, sha256 {sha256}")


if __name__ == "__main__":
    main()
