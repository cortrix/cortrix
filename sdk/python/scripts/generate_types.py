#!/usr/bin/env python3
"""Generate ``cortrix/types/_generated.py`` from the frozen OpenAPI spec.

Design § 4.2 calls for ``openapi-generator-cli`` (a Java tool). When that tool
is unavailable (the common case in this repo / CI), this script is the
design-§S4-permitted hand-written generator: it parses the OpenAPI 3.0 spec
directly (PyYAML), resolves local ``$ref``s across ``api/components`` and
``api/paths``, and emits a typed ``@dataclass`` per ``components.schemas`` entry.

It is intentionally dependency-light (PyYAML only) and deterministic so the
generated file can be committed and diffed.

Usage::

    python scripts/generate_types.py \
        --spec ../../api/openapi.yaml \
        --out cortrix/types/_generated.py

The resulting models are wire-faithful to the server contract; the hand-written
``cortrix/types/*.py`` modules import / re-export and Pythonic-enhance them
(``__iter__`` / ``__len__`` / defaults) per design § 4.1.
"""

from __future__ import annotations

import argparse
import keyword
import os
import re
import sys
from typing import Any, Optional

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "PyYAML is required: pip install pyyaml  (it is in the [dev] extra)\n"
    )
    raise

# OpenAPI primitive type -> Python annotation.
_PRIMITIVE = {
    "string": "str",
    "integer": "int",
    "number": "float",
    "boolean": "bool",
}


def _load_yaml(path: str) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data if isinstance(data, dict) else {}


class SpecResolver:
    """Loads the root spec and resolves local + cross-file ``$ref``s.

    Refs look like ``'../components/schemas.yaml#/components/schemas/Document'``
    or ``'#/components/schemas/Foo'``. We only need the schema name at the end of
    the JSON pointer, so resolution mostly amounts to dereferencing into already
    loaded component files.
    """

    def __init__(self, spec_path: str) -> None:
        self.spec_path = os.path.abspath(spec_path)
        self.root_dir = os.path.dirname(self.spec_path)
        self._file_cache: dict[str, dict[str, Any]] = {}
        self.root = self._load(self.spec_path)

    def _load(self, path: str) -> dict[str, Any]:
        path = os.path.abspath(path)
        if path not in self._file_cache:
            self._file_cache[path] = _load_yaml(path)
        return self._file_cache[path]

    def collect_schemas(self) -> dict[str, dict[str, Any]]:
        """Return {schema_name: schema_dict} from components/schemas.yaml."""
        # The root references './components/schemas.yaml#/...'; load that file
        # directly and take its components.schemas map (the authoritative source).
        schemas_file = os.path.join(self.root_dir, "components", "schemas.yaml")
        doc = self._load(schemas_file)
        out = doc.get("components", {}).get("schemas", {})
        return {k: v for k, v in out.items() if isinstance(v, dict)}

    @staticmethod
    def ref_name(ref: str) -> str:
        """'.../schemas.yaml#/components/schemas/Document' -> 'Document'."""
        return ref.rsplit("/", 1)[-1]


def _sanitize(name: str) -> str:
    safe = re.sub(r"[^0-9a-zA-Z_]", "_", name)
    if keyword.iskeyword(safe) or safe in {"None", "True", "False"}:
        safe += "_"
    return safe


def _annotate(schema: dict[str, Any]) -> str:
    """Map an OpenAPI schema fragment to a Python annotation string."""
    if "$ref" in schema:
        return f'"{SpecResolver.ref_name(schema["$ref"])}"'

    enum = schema.get("enum")
    if enum and all(isinstance(e, str) for e in enum):
        inner = ", ".join(f'"{e}"' for e in enum)
        return f"Literal[{inner}]"

    typ = schema.get("type")
    if typ == "array":
        items = schema.get("items", {})
        return f"List[{_annotate(items)}]"
    if typ == "object":
        # additionalProperties / free-form -> Dict[str, Any]
        return "Dict[str, Any]"
    if typ in _PRIMITIVE:
        return _PRIMITIVE[typ]
    # Unknown / composed (oneOf/anyOf/allOf not used in this spec) -> Any
    return "Any"


def _emit_dataclass(name: str, schema: dict[str, Any]) -> str:
    props: dict[str, Any] = schema.get("properties", {})
    required = set(schema.get("required", []))
    desc = schema.get("description", "")

    lines: list[str] = ["@dataclass", f"class {_sanitize(name)}:"]
    doc = f'    """{desc.strip().splitlines()[0]}"""' if desc else f'    """{name} (generated)."""'
    lines.append(doc)

    if not props:
        lines.append("    pass")
        return "\n".join(lines) + "\n"

    # Required fields first (no default), then optional (= None) — dataclass rule.
    req_fields: list[str] = []
    opt_fields: list[str] = []
    for prop_name, prop_schema in props.items():
        if not isinstance(prop_schema, dict):
            prop_schema = {}
        ann = _annotate(prop_schema)
        field_name = _sanitize(prop_name)
        is_required = prop_name in required and not prop_schema.get("nullable", False)
        if is_required:
            req_fields.append(f"    {field_name}: {ann}")
        else:
            opt_fields.append(f"    {field_name}: Optional[{ann}] = None")

    lines.extend(req_fields)
    lines.extend(opt_fields)
    return "\n".join(lines) + "\n"


_HEADER = '''"""Auto-generated request/response models — DO NOT EDIT BY HAND.

Generated from the frozen OpenAPI spec by ``scripts/generate_types.py``
(hand-written generator standing in for openapi-generator-cli, design § S4).
Regenerate with::

    python scripts/generate_types.py

The hand-written ``cortrix/types/*.py`` modules import from here and add Pythonic
ergonomics (``__iter__`` / ``__len__`` / docstrings).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Literal, Optional

'''


def generate(spec_path: str, out_path: str) -> int:
    resolver = SpecResolver(spec_path)
    schemas = resolver.collect_schemas()
    if not schemas:
        sys.stderr.write(f"No schemas found under {spec_path}\n")
        return 1

    blocks = [_emit_dataclass(name, schema) for name, schema in schemas.items()]
    all_names = ", ".join(f'"{_sanitize(n)}"' for n in schemas)
    body = _HEADER + "\n\n".join(blocks) + f"\n\n__all__ = [{all_names}]\n"

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(body)
    sys.stdout.write(f"Generated {len(schemas)} models -> {out_path}\n")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    default_spec = os.path.normpath(os.path.join(here, "..", "..", "..", "api", "openapi.yaml"))
    default_out = os.path.normpath(os.path.join(here, "..", "cortrix", "types", "_generated.py"))
    parser.add_argument("--spec", default=default_spec, help="path to openapi.yaml")
    parser.add_argument("--out", default=default_out, help="output _generated.py path")
    args = parser.parse_args(argv)
    return generate(args.spec, args.out)


if __name__ == "__main__":
    raise SystemExit(main())
