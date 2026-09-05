#!/usr/bin/env python3
"""Fail if the public source tree contains executable enterprise coupling."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


SOURCE_SUFFIXES = {".cpp", ".cc", ".h", ".hpp"}
RULES = (
    (
        "enterprise qualified reference",
        re.compile(r"\bcortrix[.:/_-]+enterprise\b", re.IGNORECASE),
        "cortrix.enterprise",
    ),
    (
        "enterprise build macro",
        re.compile(
            r"\b(CORTRIX_ENTERPRISE|WITH_ENTERPRISE|CORTRIX_ENT|ENABLE_ENTERPRISE)\b"
        ),
        "CORTRIX_ENTERPRISE",
    ),
    (
        "enterprise conditional compilation",
        re.compile(
            r"#\s*if(?:def|ndef)?\b.*\b(ENTERPRISE|ENT_|WITH_ENT)",
            re.IGNORECASE,
        ),
        "#ifdef WITH_ENTERPRISE",
    ),
    (
        "enterprise namespace",
        re.compile(r"namespace\s+enterprise\b"),
        "namespace enterprise",
    ),
    (
        "enterprise class or interface",
        re.compile(r"\b(class|struct)\s+(I?Enterprise[A-Za-z0-9_]*)\b"),
        "class IEnterpriseConnector",
    ),
    (
        "enterprise header include",
        re.compile(
            r"#\s*include\s*[<\"][^>\"]*enterprise[^>\"]*[>\"]",
            re.IGNORECASE,
        ),
        '#include "cortrix/enterprise/connector.h"',
    ),
    (
        "enterprise extension table",
        re.compile(
            r"CREATE\s+TABLE[^;]*\b"
            r"(audit_log_extension|agent_trace_extension|interaction_sources_extension)\b",
            re.IGNORECASE,
        ),
        "CREATE TABLE audit_log_extension (id INTEGER)",
    ),
)


def collect_sources(root: Path) -> list[Path]:
    """Return the public C++ source universe in deterministic order."""
    return sorted(
        path
        for source_root in (root / "src", root / "include")
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )


def scan(root: Path) -> tuple[list[Path], list[str]]:
    """Scan the source universe and return paths plus formatted violations."""
    for label, pattern, sentinel in RULES:
        if pattern.search(sentinel) is None:
            raise RuntimeError(f"open-core scan rule is ineffective: {label}")

    sources = collect_sources(root)
    if len(sources) <= 100:
        raise RuntimeError(
            f"open-core scan expected more than 100 source files; found {len(sources)}"
        )

    if not any(
        "namespace cortrix" in path.read_text(encoding="utf-8", errors="replace")
        for path in sources
    ):
        raise RuntimeError("open-core scan did not read the CE source tree")

    violations: list[str] = []
    for path in sources:
        relative_path = path.relative_to(root)
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(),
            start=1,
        ):
            for label, pattern, _ in RULES:
                if pattern.search(line):
                    violations.append(
                        f"{relative_path}:{line_number}: [{label}] {line}"
                    )
    return sources, violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root to scan",
    )
    args = parser.parse_args()

    try:
        sources, violations = scan(args.root.resolve())
    except RuntimeError as error:
        print(error)
        return 1

    if violations:
        print("CE tree references commercial internals:")
        print("\n".join(violations))
        return 1

    print(f"isolation OK: scanned {len(sources)} CE source files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
