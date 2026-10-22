"""CortrixToolKit tests — all 29 methods, routing, transcoding, error passthrough.

Each method has at least one test. We assert (a) the method returns a plain dict
(``_to_dict`` normalized the P03 dataclass), (b) it took the expected path (P03
SDK resource verb vs HTTP fallback ``_request``), and (c) the wire contract
(path / body / params) matches the P12 mirror tool.
"""

from __future__ import annotations

import pytest

from cortrix import CortrixError
from cortrix_skills import TOOL_METHOD_NAMES, CortrixToolKit


# --- structural -------------------------------------------------------------

def test_exactly_29_methods():
    assert len(TOOL_METHOD_NAMES) == 29
    assert len(set(TOOL_METHOD_NAMES)) == 29


def test_all_names_are_callable_methods(kit):
    for name in TOOL_METHOD_NAMES:
        assert callable(getattr(kit, name)), name


def test_all_names_prefixed_cortrix():
    assert all(n.startswith("cortrix_") for n in TOOL_METHOD_NAMES)


def test_no_admin_methods():
    # F16a admin tools are explicitly out of scope for P14 (feature design 1.3).
    assert not any("admin" in n for n in TOOL_METHOD_NAMES)


# --- init / lifecycle -------------------------------------------------------

def test_init_requires_base_url_or_client():
    with pytest.raises(ValueError, match="base_url"):
        CortrixToolKit()


def test_init_lazy_no_network(fake_client):
    # Constructing with an injected client makes no calls.
    k = CortrixToolKit(client=fake_client)
    assert fake_client.calls == []
    assert fake_client.http_calls == []
    assert k is not None


def test_close_and_context_manager(fake_client):
    with CortrixToolKit(client=fake_client) as k:
        assert k is not None
    assert fake_client.closed is True


# === MVP 12 =================================================================

def test_health_uses_sdk(kit, fake_client):
    out = kit.cortrix_health()
    assert out == {"status": "ok", "version": "1.0.0-rc.2"}
    assert fake_client.names_called() == ["system.health"]


def test_query_uses_sdk_search_with_default_ns(kit, fake_client):
    out = kit.cortrix_query(query="revenue", top_k=3)
    assert isinstance(out, dict)
    assert out["results"][0]["child_id"] == "c1"  # dataclass -> dict (no model_dump)
    assert out["meta"]["coverage_ratio"] == 1.0
    name, args, kwargs = fake_client.calls[0]
    assert name == "search"
    assert args[0] == ["default"]  # default namespace applied
    assert kwargs == {"top_k": 3, "rerank": True}


def test_query_passes_explicit_namespaces(kit, fake_client):
    kit.cortrix_query(query="q", namespaces=["a", "b"])
    _, args, _ = fake_client.calls[0]
    assert args[0] == ["a", "b"]


def test_upload_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"task_id": "t1", "status": "queued"})
    out = kit.cortrix_upload(content="hello", filename="a.txt", metadata={"k": "v"})
    assert out == {"task_id": "t1", "status": "queued"}
    method, path, body, params, timeout = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/documents")
    assert body == {"namespace": "default", "content": "hello", "filename": "a.txt", "metadata": {"k": "v"}}
    assert timeout == 120.0


def test_list_documents_uses_sdk(kit, fake_client):
    out = kit.cortrix_list_documents(namespace="ns1", limit=10, offset=5)
    assert out["total"] == 1 and out["documents"][0]["document_id"] == "doc-1"
    name, args, kwargs = fake_client.calls[0]
    assert name == "documents.list"
    assert args[0] == "ns1"
    assert kwargs == {"limit": 10, "offset": 5}


def test_list_namespaces_uses_sdk(kit, fake_client):
    out = kit.cortrix_list_namespaces(limit=20)
    assert out["namespaces"][0]["namespace"] == "default"
    assert fake_client.names_called() == ["namespaces.list"]


def test_create_namespace_uses_sdk(kit, fake_client):
    out = kit.cortrix_create_namespace("proj")
    assert out["status"] == "active"
    name, args, _ = fake_client.calls[0]
    assert name == "namespaces.create" and args[0] == "proj"


def test_memory_search_uses_http_fallback_no_user_id(kit, fake_client):
    # P03 memory.search requires user_id (server-enforced in P12); keep P12 wire.
    fake_client.set_http_return({"memories": []})
    out = kit.cortrix_memory_search(query="prefs", top_k=4)
    assert out == {"memories": []}
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/memory/search")
    assert body == {"namespace": "default", "query": "prefs", "top_k": 4}


def test_log_interaction_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"interaction_id": "i1"})
    out = kit.cortrix_log_interaction(session_id="s1", query_text="hi", response_text="yo")
    assert out == {"interaction_id": "i1"}
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/interactions")
    assert body == {"namespace": "default", "session_id": "s1", "query_text": "hi", "response_text": "yo"}


def test_list_interactions_uses_http_fallback_with_filter(kit, fake_client):
    fake_client.set_http_return({"interactions": [], "total": 0})
    out = kit.cortrix_list_interactions(namespace="ns", user_id="u1", filter={"session_id": "s1"})
    assert out["total"] == 0
    method, path, _, params, _ = fake_client.http_calls[0]
    assert (method, path) == ("GET", "/interactions")
    assert params == {"namespace": "ns", "user_id": "u1", "limit": 50, "offset": 0, "filter": {"session_id": "s1"}}


def test_document_status_uses_sdk(kit, fake_client):
    out = kit.cortrix_document_status("doc-1")
    assert out["document_id"] == "doc-1" and out["status"] == "ready"
    name, args, _ = fake_client.calls[0]
    assert name == "documents.status" and args[0] == "doc-1"


def test_add_watcher_uses_sdk(kit, fake_client):
    out = kit.cortrix_add_watcher(data_dir="/data", namespace="ns", recursive=False)
    assert out["id"] == "w-1"
    name, args, kwargs = fake_client.calls[0]
    assert name == "watchers.add"
    assert args == ("/data", ["ns"])
    assert kwargs == {"recursive": False}


def test_list_watchers_uses_sdk(kit, fake_client):
    out = kit.cortrix_list_watchers()
    assert out["watchers"][0]["id"] == "w-1"
    assert fake_client.names_called() == ["watchers.list"]


# === extended 4 =============================================================

def test_cross_ns_query_uses_sdk(kit, fake_client):
    out = kit.cortrix_cross_ns_query(query="q", namespaces=["a", "b"], top_k=7)
    assert out["results"][0]["child_id"] == "c1"
    name, args, kwargs = fake_client.calls[0]
    assert name == "search" and args[0] == ["a", "b"]
    assert kwargs == {"top_k": 7, "rerank": True}


def test_async_upload_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"task_id": "t2", "status": "queued"})
    out = kit.cortrix_async_upload(content="big", namespace="ns")
    assert out["task_id"] == "t2"
    method, path, body, _, timeout = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/documents")
    assert body == {"namespace": "ns", "content": "big"}
    assert timeout == 120.0


def test_memory_search_filter_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"memories": []})
    kit.cortrix_memory_search_filter(query="q", memory_type="fact")
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/memory/search")
    assert body == {"namespace": "default", "query": "q", "top_k": 5, "memory_type": "fact"}


def test_memory_extract_trigger_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"triggered": True})
    kit.cortrix_memory_extract_trigger(session_id="s9")
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/memory/extract")
    assert body == {"namespace": "default", "session_id": "s9"}


# === new 4 ==================================================================

def test_memory_extract_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"extracted": 2})
    msgs = [{"role": "user", "content": "I prefer tea"}]
    kit.cortrix_memory_extract(messages=msgs)
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/memory/extract")
    assert body == {"namespace": "default", "messages": msgs}


def test_task_status_uses_sdk(kit, fake_client):
    out = kit.cortrix_task_status("task-1")
    assert out["task_id"] == "task-1" and out["status"] == "queued"
    name, args, _ = fake_client.calls[0]
    assert name == "documents.task_progress" and args[0] == "task-1"


def test_cancel_task_uses_sdk(kit, fake_client):
    out = kit.cortrix_cancel_task("task-1")
    assert out["status"] == "cancelled"
    name, args, _ = fake_client.calls[0]
    assert name == "documents.cancel_task" and args[0] == "task-1"


def test_query_explain_uses_http_fallback_with_explain_param(kit, fake_client):
    fake_client.set_http_return({"results": [], "meta": {}})
    kit.cortrix_query_explain(query="q", top_k=2)
    method, path, body, params, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/query")
    assert body == {"query": "q", "namespaces": ["default"], "top_k": 2, "rerank": True}
    assert params == {"explain": "true"}


# === MEM02 reverse +2 =======================================================

def test_memory_get_audit_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"entries": []})
    kit.cortrix_memory_get_audit(memory_id="mem-1", limit=10)
    method, path, _, params, _ = fake_client.http_calls[0]
    assert (method, path) == ("GET", "/memory/audit")
    assert params == {"namespace": "default", "limit": 10, "memory_id": "mem-1"}


def test_memory_revoke_fact_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"revoked": True})
    kit.cortrix_memory_revoke_fact(memory_id="mem-1")
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/memory/mem-1/revoke")
    assert body == {"namespace": "default"}


# === MEM04 reverse +1 =======================================================

def test_memory_opt_out_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"opted_out": True})
    kit.cortrix_memory_opt_out(session_id="s1", opt_out=False)
    method, path, body, _, _ = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/memory/opt-out")
    assert body == {"namespace": "default", "session_id": "s1", "opt_out": False}


# === TD-F42-BULK reverse +1 =================================================

def test_batch_submit_uses_http_fallback(kit, fake_client):
    fake_client.set_http_return({"results": [], "structured_data": {"total": 0}})
    docs = [{"doc_id": "d1", "content": "x"}]
    kit.cortrix_batch_submit(namespace="ns", documents=docs, on_duplicate="overwrite")
    method, path, body, _, timeout = fake_client.http_calls[0]
    assert (method, path) == ("POST", "/documents/batch")
    assert body == {"namespace": "ns", "documents": docs, "async": True, "on_duplicate": "overwrite"}
    assert timeout == 120.0


# === F18a reverse +1 ========================================================

def test_list_operations_uses_http_fallback_omits_none(kit, fake_client):
    fake_client.set_http_return({"operations": [], "structured_data": {"total": 0}})
    kit.cortrix_list_operations(user_id="u1", action="upload", limit=500)
    method, path, _, params, _ = fake_client.http_calls[0]
    assert (method, path) == ("GET", "/operations")
    # None-valued filters omitted; limit capped at 200.
    assert params == {"limit": 200, "user_id": "u1", "action": "upload"}


# === MEM03 reverse +4 =======================================================

def test_memory_list_uses_sdk(kit, fake_client):
    out = kit.cortrix_memory_list(memory_type="preference", include_invalidated=True, limit=300)
    assert out["memories"][0]["memory_id"] == "mem-1"
    name, args, kwargs = fake_client.calls[0]
    assert name == "memory.list"
    assert kwargs == {
        "namespace": "default",
        "memory_type": "preference",
        "include_invalidated": True,
        "limit": 200,  # capped
        "offset": 0,
    }


def test_memory_create_uses_sdk(kit, fake_client):
    out = kit.cortrix_memory_create(namespace="ns", content="likes tea", memory_type="preference")
    assert out["memory_id"] == "mem-1"
    name, args, kwargs = fake_client.calls[0]
    assert name == "memory.create"
    assert args == ("ns", "likes tea")
    assert kwargs == {"memory_type": "preference", "metadata": None}


def test_memory_edit_uses_sdk(kit, fake_client):
    out = kit.cortrix_memory_edit(memory_id="mem-1", content="updated")
    assert out["memory_id"] == "mem-1"
    name, args, kwargs = fake_client.calls[0]
    assert name == "memory.edit"
    assert args == ("mem-1",)
    assert kwargs == {"content": "updated", "metadata": None}


def test_memory_invalidate_sdk_path_when_no_optional_args(kit, fake_client):
    out = kit.cortrix_memory_invalidate(memory_id="mem-1")
    name, args, _ = fake_client.calls[0]
    assert name == "memory.invalidate" and args == ("mem-1",)
    # soft-delete semantics: status flips to invalidated (not removed).
    assert out["status"] == "invalidated"


def test_memory_invalidate_http_fallback_when_namespace_or_reason(kit, fake_client):
    fake_client.set_http_return({"memory_id": "mem-1", "status": "invalidated", "revoked_at": "2026-06-02T00:00:00Z"})
    out = kit.cortrix_memory_invalidate(memory_id="mem-1", namespace="ns", reason="obsolete")
    method, path, _, params, _ = fake_client.http_calls[0]
    assert (method, path) == ("DELETE", "/memory/mem-1")
    assert params == {"namespace": "ns", "reason": "obsolete"}
    # soft-delete: status=invalidated + revoked_at present, never hard-deleted.
    assert out["status"] == "invalidated"
    assert out["revoked_at"] == "2026-06-02T00:00:00Z"


# === GEN-Agent 4-field error passthrough ====================================

def _err() -> CortrixError:
    return CortrixError(
        "Rate limited",
        status_code=429,
        error_code="CX_ERR_RATE_LIMITED",
        retryable=True,
        category="quota",
        retry_after_ms=1500,
        structured_data={"limit": 100},
    )


def test_error_passthrough_on_sdk_path_not_rewrapped(kit, fake_client):
    # Make an SDK-verb method raise; the exact CortrixError must propagate.
    def boom(*a, **k):
        raise _err()

    fake_client.system.health = boom
    with pytest.raises(CortrixError) as ei:
        kit.cortrix_health()
    e = ei.value
    assert e.error_code == "CX_ERR_RATE_LIMITED"
    assert e.retryable is True
    assert e.category == "quota"
    assert e.retry_after_ms == 1500
    assert e.structured_data == {"limit": 100}


def test_error_passthrough_on_http_fallback(kit, fake_client):
    fake_client.set_http_error(_err())
    with pytest.raises(CortrixError) as ei:
        kit.cortrix_memory_search(query="q")
    assert ei.value.category == "quota" and ei.value.retry_after_ms == 1500


def test_mem03_error_codes_passthrough(kit, fake_client):
    fake_client.set_http_error(
        CortrixError(
            "already invalidated",
            status_code=409,
            error_code="CX_ERR_MEM03_ALREADY_INVALIDATED",
            retryable=False,
            category="permanent",
            structured_data={"memory_id": "mem-1"},
        )
    )
    with pytest.raises(CortrixError) as ei:
        kit.cortrix_memory_invalidate(memory_id="mem-1", reason="x")
    assert ei.value.error_code == "CX_ERR_MEM03_ALREADY_INVALIDATED"
    assert ei.value.retryable is False


# === _to_dict transcoder ====================================================

def test_to_dict_handles_dataclass_dict_and_none():
    from cortrix_skills.toolkit import _to_dict
    from cortrix.types import Memory

    assert _to_dict(None) is None
    assert _to_dict({"a": 1}) == {"a": 1}
    m = Memory(memory_id="m", content="c", memory_type="fact", status="valid")
    d = _to_dict(m)
    assert isinstance(d, dict) and d["memory_id"] == "m"
    lst = _to_dict([m, m])
    assert isinstance(lst, list) and lst[0]["memory_id"] == "m"
