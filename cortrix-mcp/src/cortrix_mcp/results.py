"""Compact form of a /query result for agents.

cortrix-server returns one entry per hit, each with the full metadata of its
document: ranking signals, pipeline bookkeeping and the fields set at upload.
An agent needs the evidence and what to cite, so the compact form groups hits by
document, sends each document's metadata once, keeps the fields set at upload,
drops the server's own ranking and pipeline fields, sends a parent span once even
when several hits share it, and leaves out a chunk whose text is already inside a
parent it sends. Anything that does not look like a /query result is returned
unchanged, and cortrix_query(compact=False) skips this step entirely.
"""

from __future__ import annotations

import re
from typing import Any

# Metadata the server adds while indexing or ranking; `source_path` is the name the
# document was indexed under (the server writes it over any upload value) and is
# lifted to the document. Everything else in a hit's metadata is kept.
_SERVER_PREFIXES = ("hybrid_", "doc_discovery_", "doc_summary_", "meta.", "parent_")
_SERVER_KEYS = {
    "doc_id", "source_path", "filename", "source_doc_id", "via_path", "upload_timestamp",
    "file_size_bytes", "mime_type", "page_count", "doc_language", "parse_time_ms",
}
# Server fields worth keeping when they say something.
_DESCRIPTIVE_KEYS = ("doc_title", "one_liner")
_WHITESPACE = re.compile(r"\s+")


def _uploaded_fields(metadata: dict[str, Any]) -> dict[str, Any]:
    kept = {}
    for key, value in metadata.items():
        if key in _SERVER_KEYS or key in _DESCRIPTIVE_KEYS or key.startswith(_SERVER_PREFIXES):
            continue
        kept[key] = value
    return kept


def _squeeze(text: str) -> str:
    return _WHITESPACE.sub("", text)


def compact_query_data(data: Any) -> Any:
    """Group a /query result by document; return anything else unchanged."""
    if not isinstance(data, dict) or not isinstance(data.get("results"), list):
        return data
    documents: dict[tuple, dict[str, Any]] = {}
    parents_sent: dict[tuple, set] = {}
    for hit in data["results"]:
        if not isinstance(hit, dict):
            return data
        metadata = hit.get("metadata") or {}
        key = (hit.get("namespace"), metadata.get("doc_id") or metadata.get("source_path"))
        doc = documents.get(key)
        if doc is None:
            doc = {"namespace": hit.get("namespace"), "doc_id": metadata.get("doc_id"),
                   "source_path": metadata.get("source_path")}
            for name in _DESCRIPTIVE_KEYS:
                if metadata.get(name):
                    doc[name] = metadata[name]
            doc["metadata"] = _uploaded_fields(metadata)
            doc["score"] = hit.get("rerank_score", hit.get("score"))
            doc["evidence"] = []
            documents[key] = doc
            parents_sent[key] = set()
        parent = hit.get("parent_content") or ""
        if parent:
            if hit.get("parent_id") in parents_sent[key]:
                continue
            parents_sent[key].add(hit.get("parent_id"))
            item: dict[str, Any] = {"kind": "parent", "text": parent}
            for name in ("parent_byte_start", "parent_byte_end"):
                if name in metadata:
                    item[name.removeprefix("parent_")] = int(metadata[name])
        else:
            item = {"kind": hit.get("evidence_kind") or "chunk", "text": hit.get("content", "")}
        doc["evidence"].append(item)
    for doc in documents.values():
        parents = [_squeeze(e["text"]) for e in doc["evidence"] if e["kind"] == "parent"]
        if parents:
            doc["evidence"] = [e for e in doc["evidence"]
                               if e["kind"] == "parent" or not any(_squeeze(e["text"]) in p for p in parents)]
    meta = data.get("meta") or {}
    out_meta = {key: meta[key] for key in ("namespaces_failed", "warnings", "crag_verdict") if meta.get(key)}
    if meta.get("coverage_ratio") not in (None, 1, 1.0):
        out_meta["coverage_ratio"] = meta["coverage_ratio"]
    return {"results": list(documents.values()), "meta": out_meta}
