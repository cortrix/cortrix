#!/usr/bin/env python3
"""validate_openapi_structure.py — offline OpenAPI 3.0 structural validation (no redocly / network).

Purpose
-------
redocly bundle/lint needs to fetch packages over the network. This script uses pure
Python + PyYAML for **offline** structural validation, as a skeleton-phase DoD + a fast
pre-CI check (redocly remains the final bundle/contract authority, run at S7 / when online).

Checks
------
1. All api/**/*.yaml are parseable (well-formed).
2. The main openapi.yaml has the OpenAPI 3.0 required root fields: openapi(3.0.x) / info(title,version) / paths.
3. Every $ref in the main file's paths resolves to a target file + the JSON Pointer hits (incl. RFC6901 ~0/~1 unescaping).
4. Every resolved Operation Object has responses (required by OpenAPI).
5. The top-level keys of components/* and paths/* subfiles are valid (components: / paths: / x-cortrix-extensions:).

Exit code 0 = pass; 1 = errors.

Usage:
    python3 scripts/validate_openapi_structure.py [--api-dir api]
"""
from __future__ import annotations

import argparse
import glob
import os
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write(
        "PyYAML is required: python3 -m venv .venv && .venv/bin/pip install pyyaml\n"
    )
    sys.exit(2)


def unescape_pointer_token(tok: str) -> str:
    # RFC 6901: ~1 -> '/', ~0 -> '~' (order is fixed)
    return tok.replace("~1", "/").replace("~0", "~")


def resolve_pointer(doc, pointer: str):
    """Resolve a JSON Pointer ('#/a/b' or '/a/b'); return the node if it hits, else raise KeyError."""
    if pointer in ("", "#"):
        return doc
    if pointer.startswith("#"):
        pointer = pointer[1:]
    node = doc
    for raw in pointer.split("/"):
        if raw == "":
            continue
        tok = unescape_pointer_token(raw)
        if isinstance(node, dict):
            if tok not in node:
                raise KeyError(tok)
            node = node[tok]
        elif isinstance(node, list):
            node = node[int(tok)]
        else:
            raise KeyError(tok)
    return node


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--api-dir", default=None, help="api directory (defaults to ../api relative to this script)")
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    api_dir = args.api_dir or os.path.join(here, "..", "api")
    api_dir = os.path.abspath(api_dir)

    errors: list[str] = []
    warns: list[str] = []
    parsed: dict[str, object] = {}

    # 1) Parse all YAML
    yaml_files = sorted(glob.glob(os.path.join(api_dir, "**", "*.yaml"), recursive=True))
    if not yaml_files:
        print(f"[FAIL] no yaml found: {api_dir}")
        return 1
    for p in yaml_files:
        try:
            with open(p, encoding="utf-8") as f:
                parsed[p] = yaml.safe_load(f)
        except Exception as e:  # noqa: BLE001
            errors.append(f"YAML parse failed {p}: {e}")

    main_path = os.path.join(api_dir, "openapi.yaml")
    spec = parsed.get(main_path)

    # 2) Root fields of the main file
    if not isinstance(spec, dict):
        errors.append("openapi.yaml is not a mapping or is missing")
    else:
        ver = str(spec.get("openapi", ""))
        if not ver.startswith("3.0"):
            errors.append(f"openapi version should be 3.0.x, got {ver!r}")
        info = spec.get("info")
        if not isinstance(info, dict) or "title" not in info or "version" not in info:
            errors.append("info is missing title/version")
        if "paths" not in spec or not isinstance(spec["paths"], dict):
            errors.append("missing paths (or not a mapping)")

    # 3+4) Resolve the $ref in the main file's paths → Operation.responses
    op_methods = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}
    resolved_ops = 0
    if isinstance(spec, dict) and isinstance(spec.get("paths"), dict):
        for path_key, path_item in spec["paths"].items():
            if not isinstance(path_item, dict):
                errors.append(f"paths[{path_key}] is not a mapping")
                continue
            # A path item may be {$ref: file#ptr} or an inline operation.
            target_item = path_item
            if "$ref" in path_item:
                ref = path_item["$ref"]
                file_part, _, ptr = ref.partition("#")
                ref_file = os.path.normpath(os.path.join(api_dir, file_part))
                if ref_file not in parsed:
                    try:
                        with open(ref_file, encoding="utf-8") as f:
                            parsed[ref_file] = yaml.safe_load(f)
                    except Exception as e:  # noqa: BLE001
                        errors.append(f"paths[{path_key}] $ref file could not be read {ref}: {e}")
                        continue
                try:
                    target_item = resolve_pointer(parsed[ref_file], "#" + ptr if ptr else "")
                except (KeyError, ValueError, IndexError) as e:
                    errors.append(f"paths[{path_key}] $ref pointer did not hit {ref}: {e}")
                    continue
            if not isinstance(target_item, dict):
                errors.append(f"paths[{path_key}] resolved result is not a mapping")
                continue
            for m, op in target_item.items():
                if m.lower() not in op_methods:
                    continue
                if not isinstance(op, dict):
                    errors.append(f"{path_key}.{m} operation is not a mapping")
                    continue
                if "responses" not in op:
                    errors.append(f"{path_key}.{m} is missing responses (required by OpenAPI)")
                else:
                    resolved_ops += 1

    # 5) Top-level keys of subfiles
    valid_top = {"openapi", "info", "servers", "tags", "security", "paths",
                 "components", "x-cortrix-extensions", "apis", "extends", "rules",
                 "swagger_ui"}  # swagger_ui.config.yaml = server config artifact (not an OpenAPI file)
    for p, doc in parsed.items():
        if not isinstance(doc, dict):
            warns.append(f"{os.path.relpath(p, api_dir)}: top level is not a mapping (empty file?)")
            continue
        unknown = set(doc.keys()) - valid_top
        # Allow extension keys x-*
        unknown = {k for k in unknown if not str(k).startswith("x-")}
        if unknown:
            warns.append(f"{os.path.relpath(p, api_dir)}: unusual top-level keys {sorted(unknown)}")

    # Output
    print(f"[scan] {len(yaml_files)} yaml files, {resolved_ops} operations resolved")
    for w in warns:
        print(f"[warn] {w}")
    if errors:
        for e in errors:
            print(f"[FAIL] {e}")
        print(f"\nstructural validation failed: {len(errors)} errors")
        return 1
    print("[OK] OpenAPI 3.0 structural validation passed (skeleton phase)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
