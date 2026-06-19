"""RAG retrieval via the P03 Python SDK (dogfood — design P-12 / section 5).

F48 RAG data source is cortrix-server, accessed through the P03 ``AsyncCortrix`` SDK
exactly like any external Agent (no privileged channel — design section 5.2). This
module is the single RAG seam: it calls ``client.search(namespace, query, top_k)`` and
normalises the wire-faithful ``QueryResult`` into the flat chunk shape the prompt
builder and explain meta consume.

Standalone discipline (D3 rule): during standalone development cortrix-server is not
running, so the ``AsyncCortrix.search`` coroutine is monkeypatched/stubbed in tests.
The L2 LLM-only fallback and L3 error paths (design section 9.2) live in the executor;
this module surfaces a clean ``RagResult`` (or raises) so the executor can decide.

TODO(D3.5): real cortrix-server RAG over the live /query endpoint (currently exercised
only against the stubbed SDK).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, List, Optional

import structlog

logger = structlog.get_logger()


@dataclass
class RagChunk:
    """A single retrieved chunk, normalised from a P03 ``QueryResultItem``."""

    chunk_id: str
    source_path: str
    content: str
    score: Optional[float] = None

    def to_full(self) -> dict[str, Any]:
        """B-class explain shape (chunk text + score; design section 1.7)."""
        return {
            "chunk_id": self.chunk_id,
            "source_path": self.source_path,
            "content": self.content,
            "score": self.score,
        }


@dataclass
class RagResult:
    chunks: List[RagChunk] = field(default_factory=list)
    latency_ms: int = 0

    @property
    def chunk_ids(self) -> list[str]:
        return [c.chunk_id for c in self.chunks]

    @property
    def texts(self) -> list[str]:
        return [c.content for c in self.chunks]


def _chunk_id(item: Any, idx: int) -> str:
    """Stable chunk id from a QueryResultItem (child_id, else parent_id, else index)."""
    cid = getattr(item, "child_id", None) or getattr(item, "parent_id", None)
    return str(cid) if cid else "chunk#" + str(idx)


def _source_path(item: Any) -> str:
    """Best-effort source_path from item.metadata (real-arch QueryResultItem keeps it
    in metadata, not a dedicated field)."""
    meta = getattr(item, "metadata", None) or {}
    if isinstance(meta, dict):
        return str(meta.get("source_path") or meta.get("source") or meta.get("filename") or "unknown")
    return "unknown"


def _content(item: Any) -> str:
    return str(getattr(item, "content", None) or getattr(item, "parent_content", None) or "")


def normalize_query_result(result: Any) -> RagResult:
    """Normalise a P03 ``QueryResult`` (or dict) into a :class:`RagResult`.

    Accepts both the dataclass (``result.results`` / ``result.meta``) and a plain dict
    (``result["results"]`` / ``result["meta"]``) so tests can stub either shape.
    """
    items: list[Any] = []
    latency = 0

    if result is None:
        return RagResult()

    if isinstance(result, dict):
        items = result.get("results", []) or []
        meta = result.get("meta", {}) or {}
        latency = int(meta.get("latency_ms", 0) or 0)
    else:
        items = list(getattr(result, "results", []) or [])
        meta_obj = getattr(result, "meta", None)
        if meta_obj is not None:
            latency = int(getattr(meta_obj, "latency_ms", 0) or 0)

    chunks: list[RagChunk] = []
    for idx, item in enumerate(items):
        if isinstance(item, dict):
            md = item.get("metadata")
            src_from_md = md.get("source_path") if isinstance(md, dict) else None
            cid = item.get("child_id") or item.get("parent_id") or ("chunk#" + str(idx))
            src = src_from_md or item.get("source_path") or "unknown"
            content = item.get("content") or item.get("parent_content") or item.get("chunk_text") or ""
            score = item.get("score")
        else:
            cid = _chunk_id(item, idx)
            src = _source_path(item)
            content = _content(item)
            score = getattr(item, "score", None)
        chunks.append(
            RagChunk(chunk_id=str(cid), source_path=str(src), content=str(content), score=score)
        )

    return RagResult(chunks=chunks, latency_ms=latency)


def describe_origin(name: str, source_type: str, source_ref: str, namespace: str) -> str:
    """Human/agent-readable locator for a document's source file.

    The goal (Derek 2026-06-20): no matter where a source file originally lived, the chat
    must be able to say how to find it. We surface the most specific locator available:

    * ``source_ref`` set        -> the external origin captured at ingest (e.g. an S3 URI
                                    ``s3://bucket/key`` from a connector, or a watch dir).
    * a path-like ``name``      -> the full filesystem path (watch_dir stores the resolved
                                    path in source_path), shown verbatim.
    * a bare filename (uploads) -> the canonical location IS the Cortrix store; the original
                                    is retained and addressable by namespace + filename.
    """
    st = (source_type or "").lower()
    if source_ref:
        return f"{source_type or 'external'} — {source_ref}"
    if name and ("/" in name or "\\" in name):
        return f"{source_type or 'file'} — {name}"
    if st in ("memory", "memory_session"):
        return "Cortrix conversation memory"
    return (
        f"stored in Cortrix namespace '{namespace}' (uploaded; the original is retained "
        f"in the Cortrix store and addressable by its filename)"
    )


class SdkRagProvider:
    """RAG provider backed by the P03 ``AsyncCortrix`` SDK (design P-12 dogfood).

    The data source is fixed to cortrix-server (design P-11): there is no setter for an
    alternative backend in V1.0.
    """

    def __init__(self, client: Any, namespace: str = "default", top_k: int = 5) -> None:
        self._client = client
        self.namespace = namespace
        self.top_k = top_k

    async def retrieve(self, query: str, *, namespace: Optional[str] = None) -> RagResult:
        """Run a semantic search via the SDK and normalise the result.

        Raises whatever the SDK raises on failure (the executor maps it to the L1/L2/L3
        degradation policy in design section 9.2). ``namespace`` overrides the default
        for this request (design P-1 request-level NS override).
        """
        ns = namespace or self.namespace
        result = await self._client.search(ns, query, top_k=self.top_k)
        rag = normalize_query_result(result)
        logger.info("sdk_rag_retrieved", namespace=ns, chunks=len(rag.chunks))
        return rag

    async def list_document_summaries(
        self, *, namespace: Optional[str] = None, limit: int = 100
    ) -> List[dict[str, str]]:
        """Best-effort list of the namespace's documents so the chat can answer inventory
        AND provenance questions — "what files are here" / "list the documents" / "where
        did this come from / how do I find the source file" (RAG retrieval alone only sees
        query-relevant chunks, never the catalog or the origin).

        Each entry is ``{"name", "source_type", "origin"}`` where ``origin`` is a human-
        readable locator (full filesystem path for watch_dir, S3/connector URI from
        source_ref, or "stored in Cortrix" for managed uploads — see :func:`describe_origin`).
        Memory-session docs (chat history) are filtered out. Returns [] on any failure —
        never blocks the chat."""
        ns = namespace or self.namespace
        try:
            result = await self._client.documents.list(ns, limit=limit, offset=0)
        except Exception as exc:  # noqa: BLE001
            logger.warning("sdk_rag_list_documents_failed", namespace=ns, error=str(exc))
            return []
        docs = getattr(result, "documents", None)
        if docs is None and isinstance(result, dict):
            docs = result.get("documents", [])
        out: List[dict[str, str]] = []
        for d in docs or []:
            get = d.get if isinstance(d, dict) else (lambda k, _d=d: getattr(_d, k, None))
            source_type = get("source_type") or ""
            # Exclude conversation-memory artifacts (memory-session docs + the synthetic
            # memory-facts doc) — they are not user files.
            if source_type in ("memory_session", "memory"):
                continue
            name = get("filename") or get("source_path")
            if not name:
                continue
            source_ref = get("source_ref") or ""
            out.append(
                {
                    "name": str(name),
                    "source_type": str(source_type),
                    "origin": describe_origin(str(name), str(source_type), str(source_ref), ns),
                }
            )
        return out
