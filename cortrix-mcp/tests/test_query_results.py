"""cortrix_query's parent defaults and the compact, per-document result form."""

from __future__ import annotations

from conftest import get_tool_fn

from cortrix_mcp.results import compact_query_data
from cortrix_mcp.tools import core


def hit(doc, content, *, parent="", parent_id=None, score=0.9, ns="ns", **uploaded):
    metadata = {"doc_id": doc, "source_path": f"{doc}.md", "filename": "tmp-materialized", "hybrid_rrf_score": "0.05",
                "doc_discovery_via_path": "llm_summary", "meta.parse_status": "ok",
                "upload_timestamp": "1792546776715", "mime_type": "text/markdown",
                "doc_title": "", "one_liner": f"About {doc}.", **uploaded}
    if parent:
        metadata.update(parent_token_count="10", parent_byte_start="5", parent_byte_end="90")
    return {"namespace": ns, "content": content, "parent_content": parent,
            "parent_id": parent_id or f"p-{doc}", "child_id": f"c-{doc}-{content[:3]}",
            "content_hash": "sha256:x", "evidence_kind": "chunk", "score": 0.05,
            "rerank_score": score, "score_signals": {"semantic_score": 0.2}, "metadata": metadata}


def query_data(*hits, **meta):
    return {"results": list(hits),
            "meta": {"coverage_ratio": 1.0, "namespaces_failed": [], "warnings": [], "latency_ms": 9, **meta}}


def test_groups_hits_by_document_in_rank_order():
    data = query_data(hit("a", "first"), hit("b", "second"), hit("a", "third"))
    docs = compact_query_data(data)["results"]
    assert [d["doc_id"] for d in docs] == ["a", "b"]
    assert [e["text"] for e in docs[0]["evidence"]] == ["first", "third"]
    assert docs[0]["score"] == 0.9


def test_keeps_fields_set_at_upload_and_drops_server_fields():
    doc = compact_query_data(query_data(hit("a", "x", source_path="docs/a.md", source_sha256="f" * 64)))["results"][0]
    assert doc["metadata"] == {"source_sha256": "f" * 64}
    assert doc["source_path"] == "docs/a.md"  # the server's value, lifted to the document
    assert "filename" not in doc and "filename" not in doc["metadata"]
    assert doc["one_liner"] == "About a."
    assert "doc_title" not in doc  # empty, so left out


def test_parent_sent_once_and_chunks_inside_it_dropped():
    section = "# Title\n\nThe first chunk.\nThe second  chunk."
    data = query_data(hit("a", "The first chunk.", parent=section, parent_id="p1"),
                      hit("a", "The second chunk.", parent=section, parent_id="p1"),
                      hit("a", "Elsewhere in the file."))
    evidence = compact_query_data(data)["results"][0]["evidence"]
    assert evidence == [{"kind": "parent", "text": section, "byte_start": 5, "byte_end": 90},
                        {"kind": "chunk", "text": "Elsewhere in the file."}]


def test_chunk_inside_a_parent_is_matched_without_whitespace():
    data = query_data(hit("a", "one two", parent="x", parent_id="p1"),
                      hit("a", "line one\n\nline two", parent_id="p2"),
                      hit("a", "one   two", parent="line one\nline two", parent_id="p3"))
    kinds = [e["kind"] for e in compact_query_data(data)["results"][0]["evidence"]]
    assert kinds == ["parent", "parent"]


def test_meta_keeps_only_what_the_agent_acts_on():
    out = compact_query_data(query_data(hit("a", "x"), warnings=[{"code": "CX_WARN_RERANK_DISABLED"}]))
    assert out["meta"] == {"warnings": [{"code": "CX_WARN_RERANK_DISABLED"}]}
    partial = compact_query_data(query_data(hit("a", "x"), coverage_ratio=0.5, namespaces_failed=[{"namespace": "b"}]))
    assert partial["meta"] == {"coverage_ratio": 0.5, "namespaces_failed": [{"namespace": "b"}]}


def test_other_shapes_pass_through_unchanged():
    for data in ({"data": [{"score": 0.9}]}, [], {"results": "x"}, {"results": [1, 2]}):
        assert compact_query_data(data) == data


def test_query_asks_for_one_parent_by_default(mock_request):
    mock_request.set(json_body=query_data(hit("a", "x")))
    get_tool_fn("cortrix_query")(query="hi")
    body = mock_request.last_call.kwargs["json"]
    assert body["include_parent"] is True
    assert body["parent_expand_top_k"] == core.PARENT_EXPAND_TOP_K == 1
    assert body["parent_max_bytes"] == core.PARENT_MAX_BYTES == 16384


def test_query_without_parent_sends_no_parent_fields(mock_request):
    mock_request.set(json_body=query_data(hit("a", "x")))
    get_tool_fn("cortrix_query")(query="hi", include_parent=False)
    body = mock_request.last_call.kwargs["json"]
    assert not {"include_parent", "parent_expand_top_k", "parent_max_bytes"} & set(body)


def test_query_returns_compact_data_by_default(mock_request):
    mock_request.set(json_body=query_data(hit("a", "x", source_path="docs/a.md")))
    out = get_tool_fn("cortrix_query")(query="hi")
    assert out["data"]["results"][0]["source_path"] == "docs/a.md"
    assert out["data"]["results"][0]["metadata"] == {}
    assert out["meta"]["category"] == "success"


def test_query_compact_false_returns_the_server_result(mock_request):
    raw = query_data(hit("a", "x"))
    mock_request.set(json_body=raw)
    out = get_tool_fn("cortrix_query")(query="hi", compact=False)
    assert out["data"] == raw
