"""Find indexed documents by path: the matching rules behind ``cortrix_find_documents``.

A document's name in the index is the ``filename`` the server recorded at upload, the
same value a ``cortrix_query`` hit carries as ``source_path``. A requested path is
matched first exactly, then by its last segment (``match: "basename"``, at most 20
documents of that name listed, with ``truncated`` set when more exist); a path with
neither match goes to ``not_found``. ``match_count`` is the real number of matching
documents. A document with no recorded name (empty ``filename``) is never matched.
Nothing is normalized: case, a leading ``./`` and the separator are compared as given,
so two different files are never taken for one.
"""

from __future__ import annotations

import re
from typing import Any

PAGE_SIZE = 200  # the server's per-page maximum on GET /documents
MAX_PAGES = 50  # at most 10,000 documents scanned in one call
MAX_PATHS = 100
MAX_BASENAME_MATCHES = 20  # documents kept per path for a basename match
_FULL_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_PREFIX128 = re.compile(r"^sha256:[0-9a-f]{32}$")


def content_hash_kind(value: Any) -> str:
    """Classify a document's ``content_hash`` by its form.

    ``"sha256"``: the full digest of the uploaded file's bytes (a multipart upload or a
    watched directory). ``"sha256_prefix128"``: ``sha256:`` plus the first 32 hex
    characters of the digest of submitted text (``POST /documents``, batch, database
    import), which is not the original file's digest. ``"unknown"``: empty or any
    other form.
    """
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
    """A ``Document`` as the list endpoint returns it, without ``namespace``, plus the hash kind."""
    view = {k: v for k, v in doc.items() if k != "namespace"}
    view["content_hash_kind"] = content_hash_kind(doc.get("content_hash"))
    return view


def match_documents(paths: list[str], documents: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[str]]:
    """Match each requested path against the scanned documents.

    Returns ``(results, not_found)``: one result per path, in the order given, with
    ``match`` as ``"exact"``, ``"basename"`` or ``"none"``, ``match_count`` and the
    matching documents: all of them for an exact match, the first 20 for a basename
    match, which then carries ``"truncated": True`` when more were cut.
    """
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
