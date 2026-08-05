"""Unit tests for the Python SDK RAG seam (agent_core.sdk_rag)."""

import pytest

from agent_core.sdk_rag import (
    RagChunk,
    RagResult,
    SdkRagProvider,
    normalize_query_result,
)


def test_ragchunk_to_full():
    c = RagChunk(chunk_id="c1", source_path="/a.pdf", content="text", score=0.9)
    assert c.to_full() == {
        "chunk_id": "c1",
        "source_path": "/a.pdf",
        "content": "text",
        "score": 0.9,
    }


def test_ragresult_properties():
    r = RagResult(
        chunks=[RagChunk("c1", "/a", "x"), RagChunk("c2", "/b", "y")], latency_ms=12
    )
    assert r.chunk_ids == ["c1", "c2"]
    assert r.texts == ["x", "y"]


def test_normalize_none_returns_empty():
    r = normalize_query_result(None)
    assert r.chunks == [] and r.latency_ms == 0


def test_normalize_dict_shape_and_fallbacks():
    raw = {
        "results": [
            {"child_id": "d1#0", "metadata": {"source_path": "/doc1.pdf"}, "content": "hello", "score": 0.8},
            {"parent_id": "d2", "source_path": "/doc2.pdf", "parent_content": "world"},
            {"chunk_text": "fallback text"},
        ],
        "meta": {"latency_ms": 45},
    }
    r = normalize_query_result(raw)
    assert r.latency_ms == 45
    assert len(r.chunks) == 3
    assert (r.chunks[0].chunk_id, r.chunks[0].source_path, r.chunks[0].content) == ("d1#0", "/doc1.pdf", "hello")
    assert (r.chunks[1].chunk_id, r.chunks[1].source_path, r.chunks[1].content) == ("d2", "/doc2.pdf", "world")
    assert r.chunks[2].chunk_id == "chunk#2"  # no id -> index fallback
    assert r.chunks[2].content == "fallback text"
    assert r.chunks[2].source_path == "unknown"


class _Item:
    """Object-shaped QueryResultItem stub (the non-dict normalize path)."""

    def __init__(self, **kw):
        for k, v in kw.items():
            setattr(self, k, v)


class _Meta:
    def __init__(self, latency_ms):
        self.latency_ms = latency_ms


class _Result:
    def __init__(self, results, meta=None):
        self.results = results
        self.meta = meta


def test_normalize_dataclass_shape():
    res = _Result(
        results=[_Item(child_id="x1", metadata={"source": "/s.pdf"}, content="c", score=0.5)],
        meta=_Meta(latency_ms=20),
    )
    r = normalize_query_result(res)
    assert r.latency_ms == 20
    assert (r.chunks[0].chunk_id, r.chunks[0].source_path, r.chunks[0].content) == ("x1", "/s.pdf", "c")


def test_normalize_dataclass_fallbacks():
    """parent_id -> id; filename -> source; parent_content -> content."""
    res = _Result(results=[_Item(parent_id="p1", metadata={"filename": "f.txt"}, parent_content="pc")])
    r = normalize_query_result(res)
    assert (r.chunks[0].chunk_id, r.chunks[0].source_path, r.chunks[0].content) == ("p1", "f.txt", "pc")


def test_normalize_dataclass_index_and_unknown_fallback():
    res = _Result(results=[_Item(content="only")])
    r = normalize_query_result(res)
    assert (r.chunks[0].chunk_id, r.chunks[0].source_path, r.chunks[0].content) == ("chunk#0", "unknown", "only")


@pytest.mark.asyncio
async def test_provider_retrieve_calls_search():
    class _Client:
        def __init__(self):
            self.called = None

        async def search(self, ns, query, top_k=5):
            self.called = (ns, query, top_k)
            return {"results": [{"child_id": "c", "content": "x"}], "meta": {"latency_ms": 1}}

    client = _Client()
    p = SdkRagProvider(client, namespace="default", top_k=3)
    r = await p.retrieve("q")
    assert client.called == ("default", "q", 3)
    assert len(r.chunks) == 1


@pytest.mark.asyncio
async def test_provider_retrieve_namespace_override():
    class _Client:
        async def search(self, ns, query, top_k=5):
            assert ns == "legal"
            return {"results": [], "meta": {}}

    p = SdkRagProvider(_Client(), namespace="default")
    r = await p.retrieve("q", namespace="legal")
    assert r.chunks == []
