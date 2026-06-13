"""Type-layer tests: generated models, tolerant parsing, list ergonomics."""

from __future__ import annotations

import pytest

from cortrix._models import parse_model
from cortrix.types import (
    Document,
    DocumentList,
    Memory,
    Namespace,
    NamespaceList,
    QueryResult,
)


def test_parse_ignores_unknown_keys() -> None:
    # forward-compat: server adds a field we don't model -> ignored, no error
    d = parse_model(Document, {"document_id": "d1", "status": "ready", "future_field": 123})
    assert d.document_id == "d1" and d.status == "ready"


def test_parse_fills_optional_missing_with_none() -> None:
    d = parse_model(Document, {"document_id": "d1", "status": "ready"})
    assert d.filename is None and d.namespace is None and d.progress is None


def test_parse_nested_dataclass() -> None:
    d = parse_model(
        Document,
        {"document_id": "d1", "status": "ready",
         "progress": {"stage": "embedding", "chunks_total": 10, "chunks_done": 4}},
    )
    assert d.progress is not None
    assert d.progress.stage == "embedding" and d.progress.chunks_done == 4


def test_parse_list_of_dataclass() -> None:
    q = parse_model(
        QueryResult,
        {"results": [{"child_id": "c1", "content": "a", "score": 0.1, "namespace": "n"},
                     {"child_id": "c2", "content": "b", "score": 0.2, "namespace": "n"}],
         "meta": {"namespaces_queried": ["n"], "namespaces_succeeded": ["n"],
                  "coverage_ratio": 1.0, "latency_ms": 3}},
    )
    assert len(q.results) == 2 and q.results[1].child_id == "c2"
    assert q.meta.coverage_ratio == 1.0


def test_parse_required_field_missing_raises() -> None:
    with pytest.raises(TypeError):
        parse_model(Memory, {"content": "x"})  # missing required memory_id/memory_type/status


def test_namespace_list_iter_len() -> None:
    nl = NamespaceList(namespaces=[Namespace(namespace="a", status="active"),
                                   Namespace(namespace="b", status="active")], total=2)
    assert len(nl) == 2 and [n.namespace for n in nl] == ["a", "b"]


def test_document_list_iter_len() -> None:
    dl = DocumentList(documents=[Document(document_id="d1", status="ready")], total=1)
    assert len(dl) == 1 and list(dl)[0].document_id == "d1"


def test_generated_module_exports_34_models() -> None:
    from cortrix.types import _generated

    assert len(_generated.__all__) == 34
    assert "QueryResult" in _generated.__all__ and "GcStatus" in _generated.__all__
