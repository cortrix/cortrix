"""CortrixToolKit.cortrix_find_documents: paging, fields and the same matching rules as the MCP tool."""

from __future__ import annotations

import pytest
from cortrix import CortrixError
from cortrix.types import Document
from cortrix.types.lists import DocumentList

from cortrix_skills import _lookup

FULL = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
SHORT = "sha256:9f86d081884c7d659a2feaa0c55ad015"


def doc(ident, filename, content_hash=FULL, **extra):
    return Document(document_id=ident, status="ready", namespace="docs", filename=filename,
                    content_hash=content_hash, metadata=extra.pop("metadata", {}), **extra)


def serve(fake_client, pages_by_offset):
    """Answer documents.list from ``{offset: DocumentList}``; an exception value is raised."""
    def answer(ns, limit, offset):
        page = pages_by_offset[offset]
        if isinstance(page, BaseException):
            raise page
        return page
    fake_client.documents.list._return_value = answer


def list_calls(fake_client):
    return [c[2] for c in fake_client.calls if c[0] == "documents.list"]


def test_pages_by_the_page_size_and_stops_at_total(kit, fake_client):
    first = [doc(f"a{i}", f"f{i}.md") for i in range(200)]
    second = [doc(f"b{i}", f"g{i}.md") for i in range(200)]
    serve(fake_client, {0: DocumentList(documents=first, total=400), 200: DocumentList(documents=second, total=400)})
    out = kit.cortrix_find_documents(paths=["g5.md"], namespace="docs")
    assert list_calls(fake_client) == [{"limit": 200, "offset": 0}, {"limit": 200, "offset": 200}]
    assert out["scan_complete"] is True and out["scanned"] == 400 and out["total"] == 400
    assert out["results"][0]["match"] == "exact"


def test_offset_advances_by_the_page_size_even_on_a_short_page(kit, fake_client):
    serve(fake_client, {0: DocumentList(documents=[doc("1", "a.md")], total=300),
                        200: DocumentList(documents=[doc("2", "b.md")], total=300)})
    out = kit.cortrix_find_documents(paths=["b.md"])
    assert [c["offset"] for c in list_calls(fake_client)] == [0, 200]
    assert out["scan_complete"] is True and out["scanned"] == 2 and out["not_found"] == []


def test_document_fields_pass_through_unchanged(kit, fake_client):
    full = Document(document_id="d1", status="ready", namespace="docs", filename="dir/a.md", source_type="upload",
                    source_ref="", content_hash=FULL, metadata=None, created_at="2026-10-01T00:00:00Z")
    serve(fake_client, {0: DocumentList(documents=[full], total=1)})
    got = kit.cortrix_find_documents(paths=["dir/a.md"])["results"][0]["documents"][0]
    assert set(got) == {"document_id", "status", "filename", "source_type", "source_ref", "content_hash",
                        "metadata", "created_at", "content_hash_kind"}
    assert "namespace" not in got and "progress" not in got
    assert got["metadata"] is None
    assert got["document_id"] == "d1" and got["source_type"] == "upload" and got["source_ref"] == ""
    assert got["created_at"] == "2026-10-01T00:00:00Z" and got["content_hash"] == FULL
    assert got["content_hash_kind"] == "sha256"


def test_hash_kinds(kit, fake_client):
    docs = [doc("3", "full.md"), doc("2", "short.md", SHORT, metadata={"source_sha256": "f" * 64}),
            doc("1", "none.md", None)]
    serve(fake_client, {0: DocumentList(documents=docs, total=3)})
    results = kit.cortrix_find_documents(paths=["full.md", "short.md", "none.md"])["results"]
    kinds = [r["documents"][0]["content_hash_kind"] for r in results]
    assert kinds == ["sha256", "sha256_prefix128", "unknown"]
    assert results[1]["documents"][0]["metadata"] == {"source_sha256": "f" * 64}


def test_basename_matches_are_cut_at_twenty_with_the_real_count(kit, fake_client):
    many = [doc(str(100 - i), f"dir{i}/README.md") for i in range(25)]
    few = [doc(str(50 - i), f"pkg{i}/setup.py") for i in range(3)]
    serve(fake_client, {0: DocumentList(documents=many + few, total=28)})
    cut, kept = kit.cortrix_find_documents(paths=["README.md", "setup.py"])["results"]
    assert cut["match"] == "basename" and cut["match_count"] == 25 and cut["truncated"] is True
    assert [d["document_id"] for d in cut["documents"]] == [str(100 - i) for i in range(20)]
    assert kept["match_count"] == 3 and len(kept["documents"]) == 3 and "truncated" not in kept


def test_several_exact_matches_for_one_path_are_all_listed(kit, fake_client):
    docs = [doc(str(30 - i), "notes/a.md") for i in range(25)]
    serve(fake_client, {0: DocumentList(documents=docs, total=25)})
    entry = kit.cortrix_find_documents(paths=["notes/a.md"])["results"][0]
    assert entry["match"] == "exact" and entry["match_count"] == 25 and len(entry["documents"]) == 25
    assert "truncated" not in entry


def test_documents_without_a_filename_are_never_matched(kit, fake_client):
    serve(fake_client, {0: DocumentList(documents=[doc("1", ""), doc("2", None), doc("3", "dir/x.md")], total=3)})
    out = kit.cortrix_find_documents(paths=["", "y.md", "x.md"])
    assert [r["match"] for r in out["results"]] == ["none", "none", "basename"]
    assert [r["match_count"] for r in out["results"]] == [0, 0, 1]
    assert out["not_found"] == ["", "y.md"] and out["scanned"] == 3


def test_a_failing_page_fails_the_whole_call(kit, fake_client):
    down = CortrixError("unavailable", status_code=503, error_code="CX_ERR_BACKEND_UNAVAILABLE",
                        retryable=True, category="transient")
    first = [doc("1", "a.md")] + [doc(f"x{i}", f"x{i}.md") for i in range(199)]
    serve(fake_client, {0: DocumentList(documents=first, total=300), 200: down})
    with pytest.raises(CortrixError) as ei:
        kit.cortrix_find_documents(paths=["a.md"])
    assert ei.value is down


def test_dedupes_documents_repeated_across_pages(kit, fake_client):
    first = [doc(str(i), f"f{i}.md") for i in range(200)]
    second = [doc("199", "f199.md"), doc("200", "f200.md")]
    serve(fake_client, {0: DocumentList(documents=first, total=202), 200: DocumentList(documents=second, total=202)})
    out = kit.cortrix_find_documents(paths=["f199.md", "f200.md"])
    assert out["scanned"] == 201 and out["scan_complete"] is True
    assert [r["match_count"] for r in out["results"]] == [1, 1]


def test_total_none_pages_until_a_short_page(kit, fake_client):
    serve(fake_client, {0: DocumentList(documents=[doc(f"a{i}", f"a{i}.md") for i in range(200)], total=None),
                        200: DocumentList(documents=[doc(f"b{i}", f"b{i}.md") for i in range(200)], total=None),
                        400: DocumentList(documents=[doc(f"c{i}", f"c{i}.md") for i in range(50)], total=None)})
    out = kit.cortrix_find_documents(paths=["c49.md"])
    assert [c["offset"] for c in list_calls(fake_client)] == [0, 200, 400]
    assert out["scan_complete"] is True and out["scanned"] == 450 and out["total"] is None
    assert out["results"][0]["match"] == "exact"


def test_total_none_stops_at_the_page_cap_as_incomplete(kit, fake_client, monkeypatch):
    monkeypatch.setattr(_lookup, "MAX_PAGES", 2)
    serve(fake_client, {0: DocumentList(documents=[doc(f"a{i}", f"a{i}.md") for i in range(200)], total=None),
                        200: DocumentList(documents=[doc(f"b{i}", f"b{i}.md") for i in range(200)], total=None)})
    out = kit.cortrix_find_documents(paths=["zzz.md"])
    assert len(list_calls(fake_client)) == 2
    assert out["scan_complete"] is False and out["scanned"] == 400 and out["not_found"] == ["zzz.md"]


def test_known_total_stops_at_the_page_cap_as_incomplete(kit, fake_client, monkeypatch):
    monkeypatch.setattr(_lookup, "MAX_PAGES", 2)
    serve(fake_client, {0: DocumentList(documents=[doc(f"a{i}", f"a{i}.md") for i in range(200)], total=1000),
                        200: DocumentList(documents=[doc(f"b{i}", f"b{i}.md") for i in range(200)], total=1000)})
    out = kit.cortrix_find_documents(paths=["zzz.md"])
    assert len(list_calls(fake_client)) == 2
    assert out["scan_complete"] is False and out["scanned"] == 400 and out["total"] == 1000
