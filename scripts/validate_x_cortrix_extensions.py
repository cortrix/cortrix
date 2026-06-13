#!/usr/bin/env python3
"""validate_x_cortrix_extensions.py — validate x-cortrix-* field compliance in paths/*.yaml (P04 § 3.3).

CI-enforced (P04 § 3.3 + D7 addendum): every x-cortrix-* field used in paths/*.yaml must be
defined in components/x-cortrix.yaml, with enum values inside the allowed set and pattern
fields matching the format.

Offline, pure Python + PyYAML (no network / no redocly). Exit code 0=pass, 1=violations.

Usage:
    python3 scripts/validate_x_cortrix_extensions.py \
        [--extensions-schema api/components/x-cortrix.yaml] [--paths-dir api/paths]
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write("PyYAML is required\n")
    sys.exit(2)

METHODS = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}


def load_schema(path: str) -> dict:
    doc = yaml.safe_load(open(path, encoding="utf-8"))
    ext = (doc or {}).get("x-cortrix-extensions", {})
    if not isinstance(ext, dict):
        raise SystemExit(f"[FAIL] {path} is missing the x-cortrix-extensions top-level key")
    return ext


def walk_x_cortrix(node, schema: dict, where: str, errors: list):
    """Validate all x-cortrix-* fields within a single operation dict."""
    for key, val in node.items():
        if not (isinstance(key, str) and key.startswith("x-cortrix-")):
            continue
        spec = schema.get(key)
        if spec is None:
            errors.append(f"{where}: '{key}' is not defined in x-cortrix.yaml")
            continue
        typ = spec.get("type")
        # enum check
        if "enum" in spec and val not in spec["enum"]:
            errors.append(f"{where}: '{key}' value {val!r} not in enum {spec['enum']}")
        # pattern check (string)
        if typ == "string" and "pattern" in spec and isinstance(val, str):
            if not re.match(spec["pattern"], val):
                errors.append(f"{where}: '{key}' value {val!r} does not match pattern {spec['pattern']!r}")
        # boolean check
        if typ == "boolean" and not isinstance(val, bool):
            errors.append(f"{where}: '{key}' should be boolean, got {type(val).__name__}")
        # object (typical-latency) check of required subkeys
        if typ == "object" and isinstance(val, dict) and "required" in spec:
            for rk in spec["required"]:
                if rk not in val:
                    errors.append(f"{where}: '{key}' is missing required subkey '{rk}'")


def main() -> int:
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--extensions-schema", default=os.path.join(here, "..", "api", "components", "x-cortrix.yaml"))
    ap.add_argument("--paths-dir", default=os.path.join(here, "..", "api", "paths"))
    args = ap.parse_args()

    schema = load_schema(os.path.abspath(args.extensions_schema))
    paths_dir = os.path.abspath(args.paths_dir)

    errors: list[str] = []
    n_ops = 0
    n_fields = 0

    for pf in sorted(glob.glob(os.path.join(paths_dir, "*.yaml"))):
        doc = yaml.safe_load(open(pf, encoding="utf-8")) or {}
        rel = os.path.basename(pf)
        for path_key, item in (doc.get("paths") or {}).items():
            if not isinstance(item, dict):
                continue
            for m, op in item.items():
                if m.lower() not in METHODS or not isinstance(op, dict):
                    continue
                n_ops += 1
                n_fields += sum(1 for k in op if isinstance(k, str) and k.startswith("x-cortrix-"))
                walk_x_cortrix(op, schema, f"{rel} {m.upper()} {path_key}", errors)

    print(f"[scan] {n_ops} operations, {n_fields} x-cortrix-* field references, schema defines {len(schema)} fields")
    if errors:
        for e in errors:
            print(f"[FAIL] {e}")
        print(f"\nx-cortrix-* validation failed: {len(errors)} violations")
        return 1
    print("[OK] all x-cortrix-* fields compliant (defined + enum/pattern checks passed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
