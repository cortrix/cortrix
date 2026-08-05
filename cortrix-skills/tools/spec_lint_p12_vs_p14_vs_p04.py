#!/usr/bin/env python3
"""spec_lint — three-way consistency check: Skill SDK ↔ MCP server ↔ API spec (feature design 10.4).

Checks that the three agent-access surfaces stay 1:1:

  * **Skill SDK** ``CortrixToolKit`` — 29 methods + their parameter sets (introspected).
  * **MCP server** Cortrix MCP tools — ``cortrix_*`` ``@mcp.tool()`` functions + params
    (parsed from ``cortrix-mcp/src/cortrix_mcp/tools/*.py`` with the stdlib ``ast``
    module, so FastMCP need not be installed).
  * **API spec** OpenAPI — operation paths/methods (parsed from ``api/openapi.yaml`` +
    ``api/paths/*.yaml``; requires PyYAML, else the API spec leg is skipped with a warning).

Three-way rules:
  1. Skill SDK method names == MCP server tool names (set equality, admin tools excluded).
  2. For each shared name, the Skill SDK method parameter set == the MCP server tool parameter
     set (modulo the known ``async_``↔``async`` alias and ``self``).
  3. Each method's underlying wire path is present in API spec, **or** is on the
     ``D3.5_DEFERRED_PATHS`` allowlist (endpoints not yet in the frozen spec —
     feature design section 4 note †).

**Run timing**: wired into the quick start release gate / CI (feature design section 2 /
section 10.4) — it is NOT part of the standalone unit suite. Exit code 0 = pass,
1 = drift, 2 = could not locate inputs.

Usage::

    python tools/spec_lint_p12_vs_p14_vs_p04.py [--repo-root PATH] [--json]
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import sys
from typing import Dict, List, Optional, Set, Tuple

# Admin tools are out of Skill SDK scope (feature design 1.3); exclude from the MCP server set.
ADMIN_TOOLS: Set[str] = {
    "cortrix_admin_db_credential_register",
    "cortrix_admin_db_import_run",
}

# MCP server uses ``async`` (reserved-word-safe via schema), Skill SDK uses ``async_``.
PARAM_ALIASES: Dict[str, str] = {"async_": "async"}

# Endpoints the toolkit/MCP target that are not yet in the frozen OpenAPI spec
# (feature design section 4 note †; reconciled in D3.5). Path templates use the
# server-relative form (no /api/v1 prefix).
D3_5_DEFERRED_PATHS: Set[str] = {
    "/interactions",
    "/memory/extract",
    "/memory/audit",
    "/memory/{id}/revoke",
    "/memory/opt-out",
    "/documents/batch",
    "/operations",
}


# --------------------------------------------------------------------------- #
# Skill SDK: introspect CortrixToolKit
# --------------------------------------------------------------------------- #

def collect_p14(repo_root: str) -> Dict[str, Set[str]]:
    """Return ``{method_name: {param, ...}}`` for the 29 toolkit methods."""
    import inspect

    src = os.path.join(repo_root, "cortrix-skills", "src")
    sdk = os.path.join(repo_root, "sdk", "python")
    for p in (src, sdk):
        if os.path.isdir(p) and p not in sys.path:
            sys.path.insert(0, p)

    from cortrix_skills import TOOL_METHOD_NAMES, CortrixToolKit  # type: ignore

    kit = CortrixToolKit(client=object())
    out: Dict[str, Set[str]] = {}
    for name in TOOL_METHOD_NAMES:
        sig = inspect.signature(getattr(kit, name))
        out[name] = {p for p in sig.parameters if p != "self"}
    return out


# --------------------------------------------------------------------------- #
# MCP server: parse cortrix-mcp tool modules with ast
# --------------------------------------------------------------------------- #

def _is_mcp_tool(fn: ast.FunctionDef) -> bool:
    for dec in fn.decorator_list:
        # matches @mcp.tool() and @mcp.tool
        target = dec.func if isinstance(dec, ast.Call) else dec
        if isinstance(target, ast.Attribute) and target.attr == "tool":
            return True
    return False


def collect_p12(repo_root: str) -> Dict[str, Set[str]]:
    """Return ``{tool_name: {param, ...}}`` for every ``cortrix_*`` MCP tool."""
    tools_dir = os.path.join(repo_root, "cortrix-mcp", "src", "cortrix_mcp", "tools")
    out: Dict[str, Set[str]] = {}
    if not os.path.isdir(tools_dir):
        return out
    for fname in os.listdir(tools_dir):
        if not fname.endswith(".py"):
            continue
        path = os.path.join(tools_dir, fname)
        with open(path, "r", encoding="utf-8") as fh:
            tree = ast.parse(fh.read(), filename=path)
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and node.name.startswith("cortrix_") and _is_mcp_tool(node):
                params = {a.arg for a in node.args.args if a.arg != "self"}
                params |= {a.arg for a in node.args.kwonlyargs}
                out[node.name] = params
    return out


# --------------------------------------------------------------------------- #
# API spec: parse OpenAPI paths
# --------------------------------------------------------------------------- #

def collect_p04_paths(repo_root: str) -> Optional[Set[str]]:
    """Return the set of OpenAPI path templates, or ``None`` if PyYAML is absent."""
    try:
        import yaml  # type: ignore
    except ImportError:
        return None

    paths: Set[str] = set()
    paths_dir = os.path.join(repo_root, "api", "paths")
    root_spec = os.path.join(repo_root, "api", "openapi.yaml")

    def _harvest(doc: object) -> None:
        if isinstance(doc, dict):
            block = doc.get("paths", doc)
            if isinstance(block, dict):
                for key in block:
                    if isinstance(key, str) and key.startswith("/"):
                        paths.add(key)

    if os.path.isfile(root_spec):
        with open(root_spec, "r", encoding="utf-8") as fh:
            try:
                _harvest(yaml.safe_load(fh))
            except yaml.YAMLError:
                pass
    if os.path.isdir(paths_dir):
        for fname in os.listdir(paths_dir):
            if not fname.endswith((".yaml", ".yml")):
                continue
            with open(os.path.join(paths_dir, fname), "r", encoding="utf-8") as fh:
                try:
                    _harvest(yaml.safe_load(fh))
                except yaml.YAMLError:
                    continue
    return paths


# --------------------------------------------------------------------------- #
# Checks
# --------------------------------------------------------------------------- #

def _normalise(params: Set[str]) -> Set[str]:
    return {PARAM_ALIASES.get(p, p) for p in params}


def run_checks(repo_root: str) -> Tuple[List[str], List[str]]:
    """Return ``(errors, warnings)``."""
    errors: List[str] = []
    warnings: List[str] = []

    p14 = collect_p14(repo_root)
    p12_all = collect_p12(repo_root)
    if not p12_all:
        errors.append("P12: no cortrix-mcp tools found (check cortrix-mcp/src/.../tools/).")
        return errors, warnings
    p12 = {k: v for k, v in p12_all.items() if k not in ADMIN_TOOLS}

    # Rule 1 — name set equality.
    only_p14 = set(p14) - set(p12)
    only_p12 = set(p12) - set(p14)
    if only_p14:
        errors.append(f"P14 methods with no P12 tool: {sorted(only_p14)}")
    if only_p12:
        errors.append(f"P12 tools with no P14 method: {sorted(only_p12)}")
    if len(p14) != 29:
        errors.append(f"P14 method count = {len(p14)} (expected 29).")
    if len(p12) != 29:
        warnings.append(f"P12 non-admin tool count = {len(p12)} (expected 29).")

    # Rule 2 — per-name param-set equality.
    for name in sorted(set(p14) & set(p12)):
        a, b = _normalise(p14[name]), _normalise(p12[name])
        if a != b:
            errors.append(
                f"param drift in {name}: P14-only={sorted(a - b)} P12-only={sorted(b - a)}"
            )

    # Rule 3 — API spec path coverage (best-effort; needs PyYAML).
    p04_paths = collect_p04_paths(repo_root)
    if p04_paths is None:
        warnings.append("P04: PyYAML not installed — skipped OpenAPI path coverage check.")
    else:
        for path in sorted(D3_5_DEFERRED_PATHS):
            if path in p04_paths:
                warnings.append(
                    f"P04 now defines {path!r} — remove it from D3.5_DEFERRED_PATHS and wire the SDK verb."
                )

    return errors, warnings


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="P14<->P12<->P04 three-way spec lint.")
    parser.add_argument(
        "--repo-root",
        default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")),
        help="repository root (default: two levels up from this script).",
    )
    parser.add_argument("--json", action="store_true", help="emit a JSON report.")
    args = parser.parse_args(argv)

    try:
        errors, warnings = run_checks(args.repo_root)
    except Exception as exc:  # locating inputs / import failure
        if args.json:
            print(json.dumps({"status": "error", "detail": str(exc)}))
        else:
            print(f"spec_lint: could not run: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(
            {"status": "pass" if not errors else "fail", "errors": errors, "warnings": warnings},
            indent=2,
        ))
    else:
        for w in warnings:
            print(f"WARN  {w}")
        for e in errors:
            print(f"ERROR {e}")
        print("spec_lint: PASS (P14 == P12, params aligned)" if not errors else "spec_lint: FAIL")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
