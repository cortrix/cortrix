"""Find indexed documents by path: the matching rules behind ``cortrix_find_documents``.

Mirrors the MCP tool of the same name: a requested path is matched first exactly
against the ``filename`` the server recorded at upload, then by its last segment
(``match: "basename"``, at most 20 documents of that name listed, with ``truncated``
set when more exist); a path with neither goes to ``not_found``. A document with no
recorded name (empty ``filename``) is never matched. Nothing is normalized.
"""

from __future__ import annotations

import re
from typing import Any

PAGE_SIZE = 200
MAX_PAGES = 50
MAX_PATHS = 100
MAX_BASENAME_MATCHES = 20  # documents kept per path for a basename match
_FULL_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_PREFIX128 = re.compile(r"^sha256:[0-9a-f]{32}$")


def content_hash_kind(value: Any) -> str:
    if isinstance(value, str):
        if _FULL_SHA256.match(value):
            return "sha256"
        if _PREFIX128.match(value):
            return "sha256_prefix128"
    return "unknown"


def basename(path: str) -> str:
    return re.split(r"[\\/]", path)[-1]


def unique_paths(paths: list[str]) -> list[str]:
    seen: set[str] = set()
    out = []
    for p in paths:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def document_view(doc: dict[str, Any]) -> dict[str, Any]:
    view = {k: v for k, v in doc.items() if k != "namespace"}
    view["content_hash_kind"] = content_hash_kind(doc.get("content_hash"))
    return view


def match_documents(paths: list[str], documents: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[str]]:
    exact: dict[str, list[dict[str, Any]]] = {}
    by_base: dict[str, list[dict[str, Any]]] = {}
    for doc in documents:
        name = doc.get("filename")
        if not isinstance(name, str) or not name:
            continue  # no recorded name: such a document can never be matched
        exact.setdefault(name, []).append(doc)
        by_base.setdefault(basename(name), []).append(doc)
    results, not_found = [], []
    for path in paths:
        if path in exact:
            matched = exact[path]
            results.append({"path": path, "match": "exact", "match_count": len(matched),
                            "documents": [document_view(d) for d in matched]})
        elif basename(path) in by_base:
            matched = by_base[basename(path)]
            entry = {"path": path, "match": "basename", "match_count": len(matched),
                     "documents": [document_view(d) for d in matched[:MAX_BASENAME_MATCHES]]}
            if len(matched) > MAX_BASENAME_MATCHES:
                entry["truncated"] = True
            results.append(entry)
        else:
            results.append({"path": path, "match": "none", "match_count": 0, "documents": []})
            not_found.append(path)
    return results, not_found


def find_documents_data(namespace: str, paths: list[str], documents: list[dict[str, Any]], total: int | None,
                        scanned: int, scan_complete: bool) -> dict[str, Any]:
    results, not_found = match_documents(unique_paths(paths), documents)
    return {"namespace": namespace, "results": results, "not_found": not_found, "scanned": scanned, "total": total,
            "scan_complete": scan_complete}
