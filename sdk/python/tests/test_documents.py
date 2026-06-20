"""Documents resource tests (/documents domain; real-arch wire, async upload)."""

from __future__ import annotations

import io
import json

import httpx
import pytest
import respx

from cortrix import Cortrix, CortrixError, InvalidRequestError
from cortrix.types import Document, DocumentList, DocumentTask


@respx.mock
def test_upload_filepath_sends_content_json(api_base: str, client: Cortrix, tmp_path) -> None:
    p = tmp_path / "report.txt"
    p.write_text("hello world", encoding="utf-8")
    route = respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "task_1", "status": "submitted"})
    )
    task = client.documents.upload("contracts", str(p), metadata={"src": "legal"})
    assert isinstance(task, DocumentTask) and task.task_id == "task_1"
    body = json.loads(route.calls.last.request.content)
    assert body["namespace"] == "contracts"
    assert body["content"] == "hello world"
    assert body["filename"] == "report.txt"
    assert body["metadata"] == {"src": "legal"}


@respx.mock
def test_upload_binaryio_requires_filename(client: Cortrix) -> None:
    with pytest.raises(InvalidRequestError):
        client.documents.upload("ns", io.BytesIO(b"data"))  # no filename


@respx.mock
def test_upload_binaryio_with_filename(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "t2", "status": "queued"})
    )
    client.documents.upload("ns", io.BytesIO(b"abc"), filename="a.txt")
    body = json.loads(route.calls.last.request.content)
    assert body["filename"] == "a.txt" and body["content"] == "abc"


@respx.mock
def test_upload_binary_content_base64(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "t3", "status": "queued"})
    )
    client.documents.upload("ns", io.BytesIO(b"\xff\xfe\x00"), filename="b.bin")
    body = json.loads(route.calls.last.request.content)
    # non-utf8 -> base64
    import base64

    assert base64.b64decode(body["content"]) == b"\xff\xfe\x00"


@respx.mock
def test_list_pagination_params(api_base: str, client: Cortrix) -> None:
    route = respx.get(api_base + "/documents").mock(
        return_value=httpx.Response(
            200, json={"documents": [{"document_id": "d1", "status": "ready"}], "total": 1}
        )
    )
    out = client.documents.list("ns", limit=10, offset=20)
    assert isinstance(out, DocumentList) and len(out) == 1
    assert out.documents[0].document_id == "d1"
    req = route.calls.last.request
    assert req.url.params["namespace"] == "ns"
    assert req.url.params["limit"] == "10"
    assert req.url.params["offset"] == "20"


@respx.mock
def test_get_document(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/documents/doc_9").mock(
        return_value=httpx.Response(
            200,
            json={"document_id": "doc_9", "status": "ready", "filename": "x.pdf",
                  "progress": {"stage": "ready", "chunks_total": 4, "chunks_done": 4}},
        )
    )
    doc = client.documents.get("doc_9")
    assert isinstance(doc, Document) and doc.document_id == "doc_9"
    assert doc.progress is not None and doc.progress.stage == "ready"


@respx.mock
def test_task_progress(api_base: str, client: Cortrix) -> None:
    respx.get(api_base + "/documents/tasks/task_5/progress").mock(
        return_value=httpx.Response(200, json={"task_id": "task_5", "status": "processing"})
    )
    t = client.documents.task_progress("task_5")
    assert t.status == "processing"


@respx.mock
def test_cancel_task(api_base: str, client: Cortrix) -> None:
    respx.delete(api_base + "/documents/tasks/task_5").mock(
        return_value=httpx.Response(200, json={"task_id": "task_5", "status": "cancelled"})
    )
    t = client.documents.cancel_task("task_5")
    assert t.status == "cancelled"


@respx.mock
def test_delete_returns_none(api_base: str, client: Cortrix) -> None:
    route = respx.delete(api_base + "/documents/doc_9").mock(return_value=httpx.Response(204))
    assert client.documents.delete("doc_9") is None
    assert route.calls.last.request.method == "DELETE"


@respx.mock
def test_upload_and_wait_polls_to_terminal(api_base: str, client: Cortrix, tmp_path) -> None:
    p = tmp_path / "a.txt"
    p.write_text("hi", encoding="utf-8")
    respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "t1", "status": "queued"})
    )
    prog = respx.get(api_base + "/documents/tasks/t1/progress")
    prog.side_effect = [
        httpx.Response(200, json={"task_id": "t1", "status": "processing"}),
        httpx.Response(200, json={"task_id": "t1", "status": "ready"}),
    ]
    task = client.documents.upload_and_wait("ns", str(p), poll_interval=0.0)
    assert task.status == "ready"
    assert prog.call_count == 2


@respx.mock
def test_upload_and_wait_timeout_raises(api_base: str, client: Cortrix, tmp_path) -> None:
    p = tmp_path / "a.txt"
    p.write_text("hi", encoding="utf-8")
    respx.post(api_base + "/documents").mock(
        return_value=httpx.Response(202, json={"task_id": "t1", "status": "queued"})
    )
    respx.get(api_base + "/documents/tasks/t1/progress").mock(
        return_value=httpx.Response(200, json={"task_id": "t1", "status": "processing"})
    )
    with pytest.raises(CortrixError) as ei:
        client.documents.upload_and_wait("ns", str(p), poll_interval=0.0, timeout=0.0)
    assert ei.value.category == "timeout"


# --- batch_submit (TD-F42-BULK) ---


@respx.mock
def test_batch_submit_posts_partial_success_schema(api_base: str, client: Cortrix) -> None:
    resp_body = {
        "results": [
            {"doc_id": "doc_001", "task_id": "task_1", "status": "submitted"},
        ],
        "meta": {
            "succeeded": ["doc_001"],
            "failed": [
                {
                    "doc_id": "doc_002",
                    "error_code": "CX_ERR_NS_QUOTA_EXCEEDED",
                    "category": "quota",
                    "retryable": False,
                    "retry_after_ms": None,
                    "structured_data": {"ns_quota_used": 1000, "ns_quota_limit": 1000},
                },
            ],
            "coverage_ratio": 0.5,
            "total_submitted": 2,
        },
    }
    route = respx.post(api_base + "/documents/batch").mock(
        return_value=httpx.Response(200, json=resp_body)
    )
    out = client.documents.batch_submit(
        "contracts",
        [
            {"doc_id": "doc_001", "content": "a", "metadata": {"src": "web"}},
            {"doc_id": "doc_002", "content": "b"},
        ],
    )
    # Raw partial-success envelope returned verbatim (no model coercion).
    assert out["meta"]["coverage_ratio"] == 0.5
    assert out["meta"]["failed"][0]["error_code"] == "CX_ERR_NS_QUOTA_EXCEEDED"
    body = json.loads(route.calls.last.request.content)
    assert body["namespace"] == "contracts"
    assert len(body["documents"]) == 2
    assert body["documents"][0]["doc_id"] == "doc_001"
    # Defaults: async=True, on_duplicate=skip.
    assert body["options"] == {"async": True, "on_duplicate": "skip"}


@respx.mock
def test_batch_submit_options_passthrough(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/documents/batch").mock(
        return_value=httpx.Response(200, json={"results": [], "meta": {}})
    )
    client.documents.batch_submit(
        "ns", [{"doc_id": "d1", "content": "x"}], on_duplicate="overwrite"
    )
    body = json.loads(route.calls.last.request.content)
    assert body["options"]["on_duplicate"] == "overwrite"
    assert body["options"]["async"] is True


def test_batch_submit_rejects_bad_on_duplicate(client: Cortrix) -> None:
    with pytest.raises(InvalidRequestError):
        client.documents.batch_submit(
            "ns", [{"doc_id": "d1", "content": "x"}], on_duplicate="merge"
        )


@respx.mock
def test_batch_submit_batch_level_error_raises(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/documents/batch").mock(
        return_value=httpx.Response(
            422,
            json={
                "error": {
                    "code": "CX_ERR_BATCH_SIZE_EXCEEDED",
                    "message": "documents exceeds 100",
                    "category": "permanent",
                    "retryable": False,
                }
            },
        )
    )
    with pytest.raises(CortrixError) as ei:
        client.documents.batch_submit("ns", [{"doc_id": "d1", "content": "x"}])
    assert ei.value.error_code == "CX_ERR_BATCH_SIZE_EXCEEDED"
