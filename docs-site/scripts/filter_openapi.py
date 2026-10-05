#!/usr/bin/env python3
"""Write a filtered copy of the OpenAPI spec for the documentation site.

The site renders the copy, never api/ directly. The copy describes only what
the public build serves:

- Operations that the public build answers with 404 are removed.
- Server entries other than the local one are removed.
- Edition markers and schema properties that the public build never returns
  are removed.
- Internal tracking codes and design-stage wording are removed from text,
  and so is an extension field that describes plans for later versions.
- YAML comments are dropped, because the files are re-serialized.

The script fails if anything it is meant to remove is still present, so a new
marker in the spec stops the build instead of reaching the site.

Usage: filter_openapi.py <api-dir> <output-dir>
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

import yaml

EDITION_KEY = "x-cortrix-edition"
PUBLIC_EDITION = "ce"
HTTP_METHODS = {"get", "put", "post", "delete", "patch", "head", "options", "trace"}

# Extension fields that describe plans for later versions.
DROP_KEYS = {"x-cortrix-naming-policy"}

# Schema properties that the public build never returns.
DROP_PROPERTIES = {"highlight_ranges"}

# Internal tracking codes: feature, memory, and product-surface numbers,
# deferred-work and principle tags, design stages, and section marks.
_CODE = (
    r"(?:F\d{2}[a-z]?|MEM\d{2}(?:-only)?|P(?:0\d|1[0-4])|TD-[A-Za-z0-9][A-Za-z0-9-]*|GEN-[A-Za-z]+"
    r"|ARCH|D\d(?:\.\d)?(?:/D\d(?:\.\d)?)*|topic \d+(?: [A-Z])?)"
)
_SECTION = r"§\s*[\dA-Za-z][\dA-Za-z.]*"
_BOUNDARY_L = r"(?<![A-Za-z0-9_])"
_BOUNDARY_R = r"(?![A-Za-z0-9_])"
CODE_RE = re.compile(
    rf"{_BOUNDARY_L}(?:{_CODE}(?:\s*{_SECTION})?|{_SECTION}){_BOUNDARY_R}"
)
# Design-stage wording: phase and design-version references.
STAGE_RE = re.compile(
    r"\s*\((?:Phase \d|V\d\.\d)[^)]*\)"
    r"|\s*until Phase \d(?: D\d)?(?: review)?"
    r"|\bV\d\.\d(?:\.\d)? single convention:\s*"
    r"|\bPhase \d(?: D\d)?\b"
    r"|[^.]*\b(?:removed|added|changed) in v\d\.\d(?:\.\d)?\.?"
)
STAGE_WORD_RE = re.compile(r"\bPhase \d|\bV\d\.\d\b|\bin v\d\.\d")
PAREN_REF_RE = re.compile(
    r"\s*\((?:R|AC-|S|OPEN-)\d+\)|\s*\([^()]*\b(?:rev-\d+|OBSERVABILITY)\b[^()]*\)"
)
EDITION_SENTENCE_RE = re.compile(
    r"[^.()]*\b(?:Enterprise|Cloud)\b[^.()]*(?:\([^)]*\))?[^.()]*\.?", re.IGNORECASE
)
CE_RE = re.compile(r"\bCE\b[- ]?(?:native\s+)?")
EDITION_WORD_RE = re.compile(r"\b(?:Enterprise|Cloud|CE)\b")


def clean_text(text: str, capitalize: bool = True) -> str:
    """Remove tracking codes and edition wording from one piece of text."""
    text = EDITION_SENTENCE_RE.sub(" ", text)
    text = CE_RE.sub("", text)
    text = STAGE_RE.sub("", text)
    text = PAREN_REF_RE.sub("", text)
    text = CODE_RE.sub("", text)
    # Tidy what the removals leave behind.
    text = re.sub(r"\(\s*[,;:/—]\s*", "(", text)
    text = re.sub(r"\s*[,;:/—]\s*\)", ")", text)
    text = re.sub(r",\s*,", ",", text)
    text = re.sub(r"\(\s*\)", "", text)
    text = re.sub(r"\bSee\s*(?:\+\s*)?(?:auth)?\.\s*", "", text)
    text = re.sub(r"[ \t]{2,}", " ", text)
    text = re.sub(r"[ \t]+([.,;:)])", r"\1", text)
    text = re.sub(r"\(\s+", "(", text)
    text = re.sub(r"(?m)^[ \t]+$", "", text)
    text = text.strip(" \t")
    if capitalize and text and text[0].islower() and not text.startswith(("x-", "http")):
        text = text[0].upper() + text[1:]
    return text


def is_public_operation(operation: object) -> bool:
    if not isinstance(operation, dict):
        return True
    return operation.get(EDITION_KEY, PUBLIC_EDITION) == PUBLIC_EDITION


def filter_node(node: object, key: str | None = None) -> object:
    if isinstance(node, dict):
        result = {}
        for name, value in node.items():
            if name == EDITION_KEY or name in DROP_KEYS:
                continue
            if name in HTTP_METHODS and not is_public_operation(value):
                continue
            if name == "properties" and isinstance(value, dict):
                value = {k: v for k, v in value.items() if k not in DROP_PROPERTIES}
            if name == "required" and isinstance(value, list):
                value = [item for item in value if item not in DROP_PROPERTIES]
            result[name] = filter_node(value, name)
        return result
    if isinstance(node, list):
        return [filter_node(item, key) for item in node]
    if isinstance(node, str) and key in {"description", "summary", "title"}:
        return clean_text(node)
    if isinstance(node, str) and key is not None and key.startswith("x-cortrix-"):
        # Extension values are identifiers as often as prose: leave their case alone.
        return clean_text(node, capitalize=False)
    return node


def count_operations(node: object, public_only: bool) -> int:
    total = 0
    if isinstance(node, dict):
        for name, value in node.items():
            if name in HTTP_METHODS and isinstance(value, dict) and "responses" in value:
                if not public_only or is_public_operation(value):
                    total += 1
            total += count_operations(value, public_only)
    elif isinstance(node, list):
        for value in node:
            total += count_operations(value, public_only)
    return total


def drop_empty_path_items(document: dict) -> None:
    """Remove path items that no longer hold any operation."""
    paths = document.get("paths")
    if not isinstance(paths, dict):
        return
    for path, item in list(paths.items()):
        if isinstance(item, dict) and "$ref" not in item:
            if not any(method in item for method in HTTP_METHODS):
                del paths[path]


def resolve_pointer(document: object, pointer: str) -> object:
    node = document
    for part in pointer.strip("/").split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def drop_dangling_path_refs(root: dict, documents: dict[Path, object]) -> None:
    """Remove entries of the root paths map whose target was filtered out."""
    paths = root.get("paths")
    if not isinstance(paths, dict):
        return
    for path, item in list(paths.items()):
        reference = item.get("$ref") if isinstance(item, dict) else None
        if not reference or "#" not in reference:
            continue
        file_part, pointer = reference.split("#", 1)
        target = documents.get(Path(file_part.removeprefix("./")))
        if resolve_pointer(target, pointer) is None:
            del paths[path]


def filter_servers(document: dict) -> None:
    servers = document.get("servers")
    if isinstance(servers, list):
        document["servers"] = [
            server
            for server in servers
            if isinstance(server, dict)
            and re.match(r"https?://(?:localhost|127\.0\.0\.1)", str(server.get("url", "")))
        ]


def find_leftovers(node: object, path: str, found: list[str]) -> None:
    if isinstance(node, dict):
        for name, value in node.items():
            if name == EDITION_KEY:
                found.append(f"{path}: edition marker")
            if name in DROP_KEYS:
                found.append(f"{path}: field {name}")
            if name in DROP_PROPERTIES:
                found.append(f"{path}: property {name}")
            find_leftovers(value, f"{path}/{name}", found)
    elif isinstance(node, list):
        for index, value in enumerate(node):
            find_leftovers(value, f"{path}/{index}", found)
    elif isinstance(node, str):
        leaf = path.rsplit("/", 1)[-1]
        if leaf in {"description", "summary", "title"} or leaf.startswith("x-cortrix-"):
            for regex, label in (
                (CODE_RE, "tracking code"),
                (EDITION_WORD_RE, "edition wording"),
                (STAGE_WORD_RE, "design-stage wording"),
            ):
                match = regex.search(node)
                if match:
                    found.append(f"{path}: {label} {match.group(0)!r}")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    if not (source / "openapi.yaml").is_file():
        print(f"error: {source}/openapi.yaml not found", file=sys.stderr)
        return 2
    if target.exists():
        shutil.rmtree(target)

    documents: dict[Path, object] = {}
    removed_operations = 0
    files = [source / "openapi.yaml"]
    files += sorted((source / "paths").rglob("*.yaml"))
    files += sorted((source / "components").rglob("*.yaml"))
    for file in files:
        relative = file.relative_to(source)
        document = yaml.safe_load(file.read_text(encoding="utf-8"))
        if isinstance(document, dict):
            removed_operations += count_operations(document, False) - count_operations(
                document, True
            )
            document = filter_node(document)
            drop_empty_path_items(document)
        documents[relative] = document

    root = documents[Path("openapi.yaml")]
    filter_servers(root)
    drop_dangling_path_refs(root, documents)

    leftovers: list[str] = []
    for relative, document in documents.items():
        find_leftovers(document, str(relative), leftovers)
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(
            yaml.safe_dump(document, sort_keys=False, allow_unicode=True, width=100),
            encoding="utf-8",
        )

    if leftovers:
        print("error: the filtered spec still contains internal markers:", file=sys.stderr)
        for line in leftovers:
            print(f"  {line}", file=sys.stderr)
        return 1
    print(f"filtered spec written to {target} ({removed_operations} operations removed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
