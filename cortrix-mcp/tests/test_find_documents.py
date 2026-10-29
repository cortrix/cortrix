"""cortrix_find_documents: path matching, paging and the hash kinds."""

from __future__ import annotations

import httpx
from conftest import call_tool_result, get_tool_fn, make_response

from cortrix_mcp import lookup

FULL = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
SHORT = "sha256:9f86d081884c7d659a2feaa0c55ad015"


def doc(ident, filename, content_hash=FULL, **extra):
    return {"document_id": ident, "namespace": "docs", "filename": filename, "status": "ready",
            "content_hash": content_hash, "metadata": extra.pop("metadata", {}), **extra}


def pages(mock_request, *bodies):
    mock_request.client.request.side_effect = [make_response(json_body=b) for b in bodies]


def test_exact_basename_and_none(mock_request):
    docs = [doc("1", "design/a.md"), doc("2", "notes/README.md"), doc("3", "other/README.md", SHORT, metadata={"source_sha256": "f" * 64})]
    pages(mock_request, {"documents": docs, "total": 3})
    out = get_tool_fn("cortrix_find_documents")(paths=["design/a.md", "README.md", "missing.md", "design/a.md"])
    data = out["data"]
    assert data["namespace"] == "default" and data["scan_complete"] is True and data["scanned"] == 3 and data["total"] == 3
    assert [r["match"] for r in data["results"]] == ["exact", "basename", "none"]  # the duplicate path is looked up once
    assert [r["match_count"] for r in data["results"]] == [1, 2, 0]
    assert all("truncated" not in r for r in data["results"])
    exact = data["results"][0]["documents"][0]
    assert exact["document_id"] == "1" and "namespace" not in exact and exact["content_hash_kind"] == "sha256"
    base = data["results"][1]["documents"]
    assert [d["document_id"] for d in base] == ["2", "3"]
    assert base[1]["content_hash_kind"] == "sha256_prefix128" and base[1]["metadata"] == {"source_sha256": "f" * 64}
    assert data["not_found"] == ["missing.md"]
    assert mock_request.client.request.call_args.kwargs["params"] == {"namespace": "default", "limit": 200, "offset": 0}


def test_pages_to_total_and_dedupes_repeated_documents(mock_request):
    first = [doc(str(i), f"f{i}.md") for i in range(200)]
    second = [doc("199", "f199.md"), doc("200", "f200.md")]
    pages(mock_request, {"documents": first, "total": 202}, {"documents": second, "total": 202})
    out = get_tool_fn("cortrix_find_documents")(paths=["f0.md", "f200.md", "f199.md"], namespace="docs")
    data = out["data"]
    assert data["scan_complete"] is True and data["scanned"] == 201 and data["total"] == 202
    assert [r["match"] for r in data["results"]] == ["exact", "exact", "exact"]
    assert len(data["results"][2]["documents"]) == 1
    offsets = [c.kwargs["params"]["offset"] for c in mock_request.client.request.call_args_list]
    assert offsets == [0, 200]


def test_scan_stops_at_the_page_cap(mock_request, monkeypatch):
    monkeypatch.setattr(lookup, "MAX_PAGES", 2)
    page = [doc(str(i), f"f{i}.md") for i in range(200)]
    pages(mock_request, {"documents": page, "total": 1000}, {"documents": [doc(str(200 + i), f"g{i}.md") for i in range(200)], "total": 1000})
    data = get_tool_fn("cortrix_find_documents")(paths=["zzz.md"])["data"]
    assert data["scan_complete"] is False and data["scanned"] == 400 and data["total"] == 1000
    assert data["not_found"] == ["zzz.md"]
    assert mock_request.client.request.call_count == 2


def test_hash_kinds():
    assert lookup.content_hash_kind(FULL) == "sha256"
    assert lookup.content_hash_kind(SHORT) == "sha256_prefix128"
    assert lookup.content_hash_kind("") == "unknown"
    assert lookup.content_hash_kind(None) == "unknown"
    assert lookup.content_hash_kind("SHA256:" + "a" * 32) == "unknown"


def test_a_failing_page_fails_the_whole_call(mock_request):
    mock_request.client.request.side_effect = [make_response(json_body={"documents": [doc("1", "a.md")], "total": 300}),
                                               httpx.ConnectError("down")]
    result = call_tool_result("cortrix_find_documents", paths=["a.md"])
    assert result.is_error and "CX_ERR_MCP_BACKEND_UNAVAILABLE" in result.content[0].text


def test_paths_must_be_one_to_one_hundred(mock_request):
    for paths in ([], [f"p{i}" for i in range(101)]):
        result = call_tool_result("cortrix_find_documents", paths=paths)
        assert result.is_error and "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL" in result.content[0].text
    assert not mock_request.client.request.called


def test_no_path_normalization(mock_request):
    pages(mock_request, {"documents": [doc("1", "Docs/A.md"), doc("2", "./docs/a.md")], "total": 2})
    data = get_tool_fn("cortrix_find_documents")(paths=["docs/a.md"])["data"]
    assert data["results"][0]["match"] == "basename"
    assert [d["document_id"] for d in data["results"][0]["documents"]] == ["2"]  # basename "a.md" vs "A.md": case kept


def test_basename_matches_are_cut_at_twenty_with_the_real_count(mock_request):
    many = [doc(str(100 - i), f"dir{i}/README.md") for i in range(25)]  # document_id descending, as the server lists
    few = [doc(str(50 - i), f"pkg{i}/setup.py") for i in range(3)]
    pages(mock_request, {"documents": many + few, "total": 28})
    data = get_tool_fn("cortrix_find_documents")(paths=["README.md", "setup.py"])["data"]
    cut, kept = data["results"]
    assert cut["match"] == "basename" and cut["match_count"] == 25 and cut["truncated"] is True
    assert [d["document_id"] for d in cut["documents"]] == [str(100 - i) for i in range(20)]  # the first 20 are kept
    assert kept["match"] == "basename" and kept["match_count"] == 3 and len(kept["documents"]) == 3
    assert "truncated" not in kept
    assert data["not_found"] == []


def test_documents_without_a_filename_are_never_matched(mock_request):
    docs = [doc("1", ""), doc("2", None), doc("3", "dir/x.md")]
    pages(mock_request, {"documents": docs, "total": 3})
    data = get_tool_fn("cortrix_find_documents")(paths=["", "y.md", "x.md"])["data"]
    assert [r["match"] for r in data["results"]] == ["none", "none", "basename"]
    assert [r["match_count"] for r in data["results"]] == [0, 0, 1]
    assert data["not_found"] == ["", "y.md"]  # the nameless documents are scanned but can never be found by path
    assert data["scanned"] == 3 and data["scan_complete"] is True


def test_several_exact_matches_for_one_path_are_all_listed(mock_request):
    docs = [doc(str(30 - i), "notes/a.md") for i in range(25)] + [doc("1", "other/a.md")]
    pages(mock_request, {"documents": docs, "total": 26})
    entry = get_tool_fn("cortrix_find_documents")(paths=["notes/a.md"])["data"]["results"][0]
    assert entry["match"] == "exact" and entry["match_count"] == 25
    assert [d["document_id"] for d in entry["documents"]] == [str(30 - i) for i in range(25)]  # never truncated
    assert "truncated" not in entry
