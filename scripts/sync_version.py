#!/usr/bin/env python3
"""Synchronize every Cortrix release-version surface from the root VERSION file."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SEMVER_PATTERN = re.compile(
    r"^(?P<base>[0-9]+\.[0-9]+\.[0-9]+)"
    r"(?:-(?P<prerelease>[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$"
)
SEMVER_TOKEN = r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?"
PEP440_TOKEN = r"[0-9]+\.[0-9]+\.[0-9]+(?:(?:a|b|rc)[0-9]+)?"
SHIELD_TOKEN = r"v?[0-9]+\.[0-9]+\.[0-9]+(?:--[0-9A-Za-z.]+)?"


@dataclass(frozen=True)
class Rule:
    path: str
    pattern: str
    value_kind: str
    expected_count: int = 1


RULES = (
    Rule(
        "README.md",
        rf"img\.shields\.io/badge/version-(?P<value>{SHIELD_TOKEN})-orange\.svg",
        "shield",
    ),
    Rule(
        "include/cortrix/common/version.h",
        rf'kCortrixVersion = "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "deploy/entrypoint.sh",
        rf"Cortrix v(?P<value>{SEMVER_TOKEN})",
        "semver",
    ),
    Rule("deploy/Dockerfile", rf'LABEL version="(?P<value>{SEMVER_TOKEN})"', "semver"),
    Rule("deploy/Dockerfile.cuda", rf'LABEL version="(?P<value>{SEMVER_TOKEN})"', "semver"),
    Rule(
        "api/openapi.yaml",
        rf"^  version: (?P<value>{SEMVER_TOKEN})$",
        "semver",
    ),
    Rule(
        "api/paths/system.yaml",
        rf'example: "(?P<value>{SEMVER_TOKEN})"',
        "semver",
        2,
    ),
    Rule("cortrix-agent/main.py", rf'version="(?P<value>{SEMVER_TOKEN})"', "semver"),
    Rule(
        "cortrix-mcp/pyproject.toml",
        rf'^version = "(?P<value>{PEP440_TOKEN})"$',
        "pep440",
    ),
    Rule(
        "cortrix-mcp/src/cortrix_mcp/__init__.py",
        rf'__version__ = "(?P<value>{PEP440_TOKEN})"',
        "pep440",
    ),
    Rule(
        "cortrix-mcp/Dockerfile",
        rf"cortrix-mcp==(?P<value>{PEP440_TOKEN})",
        "pep440",
    ),
    Rule(
        "cortrix-mcp/Dockerfile",
        rf"cortrix/mcp:v(?P<value>{SEMVER_TOKEN})",
        "semver",
        2,
    ),
    Rule(
        "cortrix-mcp/README.md",
        rf"cortrix/mcp:v(?P<value>{SEMVER_TOKEN})",
        "semver",
        2,
    ),
    Rule(
        "cortrix-mcp/tests/test_core.py",
        rf'"version": "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "cortrix-skills/pyproject.toml",
        rf'^version = "(?P<value>{PEP440_TOKEN})"$',
        "pep440",
    ),
    Rule(
        "cortrix-skills/pyproject.toml",
        rf"cortrix>=(?P<value>{PEP440_TOKEN}),<2\.0\.0",
        "pep440",
    ),
    Rule(
        "cortrix-skills/src/cortrix_skills/__init__.py",
        rf'__version__ = "(?P<value>{PEP440_TOKEN})"',
        "pep440",
    ),
    Rule(
        "cortrix-skills/tests/conftest.py",
        rf'"version": "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "cortrix-skills/tests/test_toolkit.py",
        rf'"version": "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "sdk/python/pyproject.toml",
        rf'^version = "(?P<value>{PEP440_TOKEN})"$',
        "pep440",
    ),
    Rule(
        "sdk/python/cortrix/_constants.py",
        rf'SDK_VERSION = "(?P<value>{PEP440_TOKEN})"',
        "pep440",
    ),
    Rule(
        "sdk/python/tests/test_extended.py",
        rf'"version": "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "sql-extensions/pgcortrix/python/pgcortrix_helper.py",
        rf'"version": "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "sql-extensions/pgcortrix/tests/test_helper.py",
        rf'\["version"\], "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "scripts/e2e_llm_sweep.py",
        rf"P6 version == (?P<value>{SEMVER_TOKEN})",
        "semver",
    ),
    Rule(
        "scripts/e2e_llm_sweep.py",
        rf'get\("version"\) == "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "tests/integration/test_health_endpoint.cpp",
        rf"version SoT \((?P<value>{SEMVER_TOKEN})\)",
        "semver",
    ),
    Rule(
        "tests/integration/test_health_endpoint.cpp",
        rf'kCortrixVersion\), "(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "tests/unit/test_http_server.cpp",
        rf"version SoT \((?P<value>{SEMVER_TOKEN})\)",
        "semver",
    ),
    Rule(
        "tests/unit/test_infra_metrics_matrix.cpp",
        rf'SetBuildInfo\("(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "tests/unit/test_infra_metrics_matrix.cpp",
        rf'version=\\"(?P<value>{SEMVER_TOKEN})\\"',
        "semver",
    ),
    Rule(
        "tests/unit/test_deploy_metrics.cpp",
        rf'SetBuildInfo\("(?P<value>{SEMVER_TOKEN})"',
        "semver",
    ),
    Rule(
        "tests/unit/test_deploy_metrics.cpp",
        rf'version=\\"(?P<value>{SEMVER_TOKEN})\\"',
        "semver",
    ),
    Rule(
        "web/package.json",
        rf'^  "version": "(?P<value>{SEMVER_TOKEN})",$',
        "semver",
    ),
    Rule(
        "web/package-lock.json",
        rf'^  "version": "(?P<value>{SEMVER_TOKEN})",$',
        "semver",
    ),
    Rule(
        "web/package-lock.json",
        rf'^    "": \{{\n      "name": "cortrix-web-ui",\n      "version": "(?P<value>{SEMVER_TOKEN})",$',
        "semver",
    ),
    Rule(
        "web/src/telemetry/metrics.ts",
        rf"VITE_APP_VERSION \?\? '(?P<value>{SEMVER_TOKEN})'",
        "semver",
    ),
    Rule(
        "web/src/api/mock.ts",
        rf"version: '(?P<value>{SEMVER_TOKEN})'",
        "semver",
        2,
    ),
    Rule(
        "web/src/components/Layout/Header.tsx",
        rf"systemStatus\?\.version \?\? '(?P<value>{SEMVER_TOKEN})'",
        "semver",
    ),
    Rule(
        "web/src/store/useAppStore.test.ts",
        rf"version: '(?P<value>{SEMVER_TOKEN})'",
        "semver",
    ),
    Rule(
        "web/src/store/useAppStore.test.ts",
        rf"version\)\.toBe\('(?P<value>{SEMVER_TOKEN})'\)",
        "semver",
    ),
)


def read_versions() -> dict[str, str]:
    lines = (ROOT / "VERSION").read_text(encoding="utf-8").splitlines()
    if len(lines) != 1 or lines[0] != lines[0].strip():
        raise ValueError("VERSION must contain exactly one unpadded line")

    version = lines[0]
    match = SEMVER_PATTERN.fullmatch(version)
    if match is None:
        raise ValueError("VERSION must be SemVer without a leading 'v'")

    base = match.group("base")
    prerelease = match.group("prerelease")
    pep440 = base
    if prerelease:
        prerelease_match = re.fullmatch(r"(alpha|beta|rc)\.([0-9]+)", prerelease)
        if prerelease_match is None:
            raise ValueError(
                "Python package versions support only alpha.N, beta.N, or rc.N prereleases"
            )
        prefix = {"alpha": "a", "beta": "b", "rc": "rc"}[prerelease_match.group(1)]
        pep440 = f"{base}{prefix}{prerelease_match.group(2)}"

    return {
        "semver": version,
        "pep440": pep440,
        "shield": f"v{version.replace('-', '--')}",
    }


def synchronize(versions: dict[str, str]) -> tuple[dict[Path, str], list[str]]:
    originals: dict[Path, str] = {}
    updated: dict[Path, str] = {}
    errors: list[str] = []

    for rule in RULES:
        path = ROOT / rule.path
        if path not in originals:
            originals[path] = path.read_text(encoding="utf-8")
            updated[path] = originals[path]

        pattern = re.compile(rule.pattern, re.MULTILINE)

        def replace(match: re.Match[str]) -> str:
            whole = match.group(0)
            start, end = match.span("value")
            relative_start = start - match.start()
            relative_end = end - match.start()
            return whole[:relative_start] + versions[rule.value_kind] + whole[relative_end:]

        updated_text, count = pattern.subn(replace, updated[path])
        if count != rule.expected_count:
            errors.append(
                f"{rule.path}: expected {rule.expected_count} match(es), found {count}: {rule.pattern}"
            )
            continue
        updated[path] = updated_text

    if errors:
        return {}, errors

    changed = {path: text for path, text in updated.items() if text != originals[path]}
    return changed, []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if any managed version surface differs from VERSION",
    )
    args = parser.parse_args()

    try:
        versions = read_versions()
        changed, errors = synchronize(versions)
    except (OSError, ValueError) as error:
        print(f"version sync error: {error}", file=sys.stderr)
        return 2

    if errors:
        for error in errors:
            print(f"version sync error: {error}", file=sys.stderr)
        return 2

    if args.check:
        if changed:
            print("Version-managed files are stale:", file=sys.stderr)
            for path in sorted(changed):
                print(f"  {path.relative_to(ROOT)}", file=sys.stderr)
            print("Run: python3 scripts/sync_version.py", file=sys.stderr)
            return 1
        print(f"All managed version surfaces match v{versions['semver']}.")
        return 0

    for path, text in changed.items():
        path.write_text(text, encoding="utf-8")
        print(f"updated {path.relative_to(ROOT)}")
    if not changed:
        print(f"All managed version surfaces already match v{versions['semver']}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
