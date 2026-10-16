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


def test_generated_module_exports_57_models() -> None:
    from cortrix.types import _generated

    assert len(_generated.__all__) == 57
    assert "QueryResult" in _generated.__all__ and "GcStatus" in _generated.__all__
    assert "SearchConfig" in _generated.__all__
    # The removed surfaces are gone with their models.
    for gone in ("SqlRequest", "SqlResult", "SyncStatus", "MemoryLogRequest"):
        assert gone not in _generated.__all__


def test_generated_types_carry_what_the_server_sends() -> None:
    import dataclasses
    import typing

    from cortrix.types import GcStatus, QueryMeta, QueryResult, QueryResultItem

    def fields(model):
        return {f.name for f in dataclasses.fields(model)}

    assert {"evidence_kind", "score_signals"} <= fields(QueryResultItem)
    assert {"via_path", "crag_verdict", "crag_action"} <= fields(QueryMeta)
    assert {"explain", "error"} <= fields(QueryResult)
    # Epoch milliseconds, as the spec and the server use it (it used to say str).
    assert typing.get_type_hints(GcStatus)["last_gc_at"] == typing.Optional[int]


# The declared task-status set must be exactly what the server can emit. The server's
# set is enforced by a CHECK constraint on the tasks table, so it cannot drift at
# runtime; what has drifted historically is this declaration.
#
# It mattered: the published spec listed `ready` and `submitted`, which the server
# never emits on this field (`ready` belongs to Document.status, a different state
# machine; `submitted` is the batch endpoint's per-item acceptance marker), and it
# omitted `completed`, which is the only terminal success value there is. A caller
# following the declaration waited for a value that never arrives, and a type checker
# reported the correct comparison -- `task.status == "completed"` -- as impossible.
def test_document_task_status_literal_is_exactly_the_server_emitted_set() -> None:
    import typing

    from cortrix.types._generated import DocumentTask

    declared = set(typing.get_args(typing.get_type_hints(DocumentTask)["status"]))
    assert declared == {
        "queued",
        "processing",
        "cancelling",
        "completed",
        "failed",
        "cancelled",
    }
    # Stated separately so a failure says which way it drifted.
    assert "completed" in declared, "the terminal success value must be declared"
    assert "ready" not in declared, "`ready` is Document.status, not DocumentTask.status"
    assert "submitted" not in declared, "`submitted` is a batch-response marker"
