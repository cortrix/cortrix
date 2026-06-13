"""Query resource — backs the top-level ``client.search()`` / ``get_sources()``.

Per Derek's ruling (briefing § 9): SDK shape = P03 § 2.12; wire = real
architecture. Since D3.5 round 2 (Wave Q) the LIVE ``POST /query`` route IS
the F04 cross-NS handler: ``namespaces`` array (single NS = one-element
list, all = ["*"]), singular ``filter``, and the § 2.12
``QueryResult{results, meta}`` response (child_id/content/content_hash
items + 8-field A-class meta). ``_adapt_wire_result`` is kept as a
tolerant shim for the pre-mount MVP response shape (chunk_text/block_id) —
F04-shaped responses pass through untouched.

``get_sources()`` -> ``GET /interactions/{id}/sources`` is **§2.12-only**
(P04 spec to be added -> D3.5).
"""

from __future__ import annotations

from typing import Any, List, Optional, Union

from ..types import QueryResult
from .._models import parse_model
from ._base import AsyncResource, SyncResource

PATH_QUERY = "/query"
PATH_INTERACTION_SOURCES = "/interactions/{id}/sources"  # §2.12-only -> D3.5 spec


def _query_body(
    namespace: Union[str, List[str]],
    query: str,
    top_k: int,
    rerank: bool,
    include_sources: bool,
    filters: Optional[dict[str, Any]],
) -> dict[str, Any]:
    namespaces = [namespace] if isinstance(namespace, str) else list(namespace)
    body: dict[str, Any] = {
        "query": query,
        "namespaces": namespaces,
        "top_k": top_k,
        "rerank": rerank,
    }
    if include_sources:
        body["include_sources"] = True
    if filters is not None:
        body["filter"] = filters  # spec field is singular ``filter``
    return body


def _adapt_wire_result(body: Any, namespace: Union[str, List[str]]) -> Any:
    """Translate the live /query response into the § 2.12 ``QueryResult`` dict.

    Field mapping (live route -> § 2.12):
      ``chunk_text`` -> ``content``; ``block_id`` -> ``child_id`` (stringified);
      ``doc_id`` -> ``parent_id``; ``source_path`` / ``block_type`` /
      ``hit_routes`` / ``vector_score`` / ``related_blocks_count`` fold into
      ``metadata``. Route-degradation info surfaces as a ``meta.warnings`` entry.
    Already-§2.12 payloads (e.g. a future F04 mount, test stubs) pass through.
    """
    if not isinstance(body, dict) or "results" not in body:
        return body
    wire_meta = body.get("meta") or {}
    if "namespaces_queried" in wire_meta:
        return body  # already the §2.12 / F04 cross-NS shape

    ns = namespace if isinstance(namespace, str) else (list(namespace) or [""])[0]
    results: list[dict[str, Any]] = []
    for it in body.get("results") or []:
        if not isinstance(it, dict):
            continue
        md = dict(it.get("metadata") or {})
        for k in ("source_path", "block_type", "hit_routes", "vector_score",
                  "related_blocks_count"):
            if k in it and k not in md:
                md[k] = it[k]
        child = it.get("block_id")
        results.append({
            "child_id": str(child) if child is not None else it.get("child_id"),
            "parent_id": it.get("doc_id") or it.get("parent_id"),
            "content": it.get("chunk_text") or it.get("content"),
            "score": it.get("score"),
            "rerank_score": it.get("rerank_score"),
            "namespace": it.get("namespace") or ns,
            "metadata": md,
        })

    warnings: Optional[list[dict[str, Any]]] = None
    if wire_meta.get("degraded"):
        warnings = [{
            "code": "ROUTES_DEGRADED",
            "degraded_routes": wire_meta.get("degraded_routes") or [],
            "degradation_level": wire_meta.get("degradation_level", 0),
        }]
    meta = {
        "namespaces_queried": [ns],
        "namespaces_succeeded": [ns],
        "coverage_ratio": 1.0,
        "latency_ms": wire_meta.get("latency_ms", 0),
        "warnings": warnings,
    }
    return {"results": results, "meta": meta}


class Query(SyncResource):
    """Semantic search. ``POST /query``."""

    def run(
        self,
        namespace: Union[str, List[str]],
        query: str,
        *,
        top_k: int = 10,
        rerank: bool = True,
        include_sources: bool = False,
        filters: Optional[dict[str, Any]] = None,
    ) -> QueryResult:
        raw = self._client._request(
            "POST",
            PATH_QUERY,
            json=_query_body(namespace, query, top_k, rerank, include_sources, filters),
        )
        return parse_model(QueryResult, _adapt_wire_result(raw, namespace))

    def get_sources(self, interaction_id: str) -> Any:
        """Citation sources for an interaction. ``GET /interactions/{id}/sources``
        (§2.12-only -> D3.5 spec)."""
        return self._client._request(
            "GET", PATH_INTERACTION_SOURCES.format(id=interaction_id)
        )


class AsyncQuery(AsyncResource):
    """Async Query (symmetric to :class:`Query`)."""

    async def run(
        self,
        namespace: Union[str, List[str]],
        query: str,
        *,
        top_k: int = 10,
        rerank: bool = True,
        include_sources: bool = False,
        filters: Optional[dict[str, Any]] = None,
    ) -> QueryResult:
        raw = await self._client._request(
            "POST",
            PATH_QUERY,
            json=_query_body(namespace, query, top_k, rerank, include_sources, filters),
        )
        return parse_model(QueryResult, _adapt_wire_result(raw, namespace))

    async def get_sources(self, interaction_id: str) -> Any:
        return await self._client._request(
            "GET", PATH_INTERACTION_SOURCES.format(id=interaction_id)
        )
