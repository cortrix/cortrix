"""Memory & operations tools: #21-#29 (feature design sections 4.4 / 4.5 / 4.5.bis /
4.5.ter / 4.5.quinquies).

  #21 cortrix_memory_get_audit  -> GET  /memory/audit            (MEM02; reuses F18a operation_log) [D3.5]
  #22 cortrix_memory_revoke_fact-> POST /memory/{id}/revoke      (MEM02 D6 self-revoke)             [D3.5]
  #23 cortrix_memory_opt_out    -> POST /memory/opt-out          (MEM04 D2+D5 opt-out / revoke)     [D3.5]
  #24 cortrix_batch_submit      -> POST /documents/batch         (TD-F42-BULK, <=100 docs, async)   [D3.5]
  #25 cortrix_list_operations   -> GET  /operations              (F18a operation_log)               [D3.5]
  #26 cortrix_memory_list       -> GET  /memory                  (MEM03 endpoint 1, P04 real)
  #27 cortrix_memory_create     -> POST /memory                  (MEM03 endpoint 2, P04 real)
  #28 cortrix_memory_edit       -> PATCH /memory/{id}            (MEM03 endpoint 3, P04 real)
  #29 cortrix_memory_invalidate -> DELETE /memory/{id}           (MEM03 endpoint 4 soft-delete, P04 real)

[D3.5] = endpoint not yet present in api/paths/*.yaml; implemented per the feature design
contract and exercised with mocked HTTP responses during standalone development.
"""

from __future__ import annotations

from typing import Optional

from ..transport import CORTRIX_NAMESPACE, request


def register(mcp) -> None:
    # ---- MEM02 reverse +2 ---------------------------------------------------
    @mcp.tool()
    def cortrix_memory_get_audit(
        memory_id: Optional[str] = None,
        namespace: str = "",
        limit: int = 50,
    ) -> dict:
        """Query the memory audit log (MEM02; reuses F18a operation_log).

        Args:
            memory_id: filter to a single memory's audit trail (optional).
            namespace: target namespace (uses default if empty).
            limit: max entries to return (default 50).

        D3.5 deferred: GET /memory/audit is not yet in api/paths/*.yaml — standalone mock.
        """
        ns = namespace or CORTRIX_NAMESPACE
        params: dict = {"namespace": ns, "limit": limit}
        if memory_id:
            params["memory_id"] = memory_id
        return request("GET", "/memory/audit", params=params, data_key="entries", timeout=10.0)

    @mcp.tool()
    def cortrix_memory_revoke_fact(
        memory_id: str,
        namespace: str = "",
    ) -> dict:
        """Self-service revoke of an auto-extracted fact (MEM02 D6).

        Marks the fact auto_revoke_eligible; the physical row is retained (full-retention
        model). D3.5 deferred: POST /memory/{id}/revoke not yet in api/paths/*.yaml — mock.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {"namespace": ns}
        return request("POST", f"/memory/{memory_id}/revoke", json_body=body, timeout=10.0)

    # ---- MEM04 reverse +1 ---------------------------------------------------
    @mcp.tool()
    def cortrix_memory_opt_out(
        session_id: str,
        opt_out: bool = True,
        namespace: str = "",
    ) -> dict:
        """Per-session memory opt-out (and opt-out revoke), MEM04 D2 + D5 combined.

        Args:
            session_id: session to opt out of memory extraction.
            opt_out: True to opt out, False to revoke the opt-out.
            namespace: target namespace (uses default if empty).

        D3.5 deferred: POST /memory/opt-out not yet in api/paths/*.yaml — standalone mock.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {"namespace": ns, "session_id": session_id, "opt_out": opt_out}
        return request("POST", "/memory/opt-out", json_body=body, timeout=10.0)

    # ---- TD-F42-BULK reverse +1 --------------------------------------------
    @mcp.tool()
    def cortrix_batch_submit(
        namespace: str,
        documents: list[dict],
        async_: bool = True,
        on_duplicate: str = "skip",
    ) -> dict:
        """Submit up to 100 documents to a namespace in one batch (TD-F42-BULK).

        Args:
            namespace: target namespace.
            documents: up to 100 items, each {doc_id, content, metadata?}.
            async_: must be True in V1 (topic 3).
            on_duplicate: skip / overwrite / error (topic 2).

        Returns a results list (same order as documents) + structured_data
        (succeeded / failed / total / coverage_ratio). A batch-level failure returns the
        GEN-Agent error response. D3.5 deferred: POST /documents/batch not yet in
        api/paths/*.yaml — standalone mock.
        """
        body = {
            "namespace": namespace,
            "documents": documents,
            "async": async_,
            "on_duplicate": on_duplicate,
        }
        return request("POST", "/documents/batch", json_body=body, timeout=120.0)

    # ---- F18a reverse +1 ----------------------------------------------------
    @mcp.tool()
    def cortrix_list_operations(
        user_id: Optional[str] = None,
        namespace: Optional[str] = None,
        action: Optional[str] = None,
        start_time: Optional[str] = None,
        end_time: Optional[str] = None,
        limit: int = 50,
        cursor: Optional[str] = None,
    ) -> dict:
        """Query the operation log (F18a; CE 30-day retention + 100K cap).

        Args:
            user_id: defaults to the current agent user_id when omitted.
            namespace: filter by namespace.
            action: one of the F18a action enum values.
            start_time / end_time: ISO 8601 time range.
            limit: max rows (default 50, max 200).
            cursor: opaque pagination token.

        Returns operations (op_id / user_id / action / namespace / details_json / trace_id /
        session_id / created_at) + structured_data (total / has_more / next_cursor).
        D3.5 deferred: GET /operations not yet in api/paths/*.yaml — standalone mock.
        """
        params: dict = {"limit": min(limit, 200)}
        for key, value in (
            ("user_id", user_id),
            ("namespace", namespace),
            ("action", action),
            ("start_time", start_time),
            ("end_time", end_time),
            ("cursor", cursor),
        ):
            if value is not None:
                params[key] = value
        return request("GET", "/operations", params=params, data_key="operations", timeout=10.0)

    # ---- MEM03 reverse +4 (real P04 endpoints) ------------------------------
    @mcp.tool()
    def cortrix_memory_list(
        namespace: Optional[str] = None,
        memory_type: Optional[str] = None,
        include_invalidated: bool = False,
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List the current user_id's memories (MEM03 endpoint 1; GET /memory).

        Args:
            namespace: defaults to the current user_id's namespace when omitted.
            memory_type: filter by type (fact / preference / event).
            include_invalidated: include soft-deleted (status=invalidated) memories for
                transparency queries.
            limit: max rows (default 50, max 200).
            offset: pagination offset.

        Errors pass through: CX_ERR_MEM03_USER_MISMATCH / CX_ERR_MEM03_INVALID_TYPE /
        CX_ERR_MEM03_QUOTA / CX_ERR_MEM03_LIST_FAILED.
        """
        ns = namespace or CORTRIX_NAMESPACE
        params: dict = {
            "namespace": ns,
            "include_invalidated": include_invalidated,
            "limit": min(limit, 200),
            "offset": offset,
        }
        if memory_type:
            params["memory_type"] = memory_type
        return request("GET", "/memory", params=params, data_key="memories", timeout=10.0)

    @mcp.tool()
    def cortrix_memory_create(
        namespace: str,
        content: str,
        memory_type: str,
        metadata: Optional[dict] = None,
    ) -> dict:
        """Create a memory (MEM03 endpoint 2; POST /memory).

        The POST path does not pass through LLM quality filtering (MEM03 topic 4 lock);
        extraction_method is set to 'user_create' server-side.

        Errors pass through: CX_ERR_MEM03_USER_MISMATCH / CX_ERR_MEM03_INVALID_TYPE /
        CX_ERR_MEM03_CONTENT_TOO_LONG / CX_ERR_MEM03_QUOTA / CX_ERR_MEM03_CREATE_FAILED.
        """
        body: dict = {"namespace": namespace, "content": content, "memory_type": memory_type}
        if metadata:
            body["metadata"] = metadata
        return request("POST", "/memory", json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_memory_edit(
        memory_id: str,
        content: Optional[str] = None,
        metadata: Optional[dict] = None,
    ) -> dict:
        """Edit an existing memory's content / metadata (MEM03 endpoint 3; PATCH /memory/{id}).

        metadata merge semantics (not replace); extraction_method is overwritten to
        'user_edit' server-side. Semantically a new user_edit memory + invalidation of the
        old one, hence PATCH not PUT.

        Errors pass through: CX_ERR_MEM03_MEMORY_NOT_FOUND / CX_ERR_MEM03_USER_MISMATCH /
        CX_ERR_MEM03_ALREADY_INVALIDATED / CX_ERR_MEM03_CONTENT_TOO_LONG /
        CX_ERR_MEM03_EDIT_FAILED / CX_ERR_MEM03_QUOTA.
        """
        body: dict = {}
        if content is not None:
            body["content"] = content
        if metadata is not None:
            body["metadata"] = metadata
        return request("PATCH", f"/memory/{memory_id}", json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_memory_invalidate(
        memory_id: str,
        namespace: Optional[str] = None,
        reason: Optional[str] = None,
    ) -> dict:
        """Soft-delete a memory (MEM03 endpoint 4; DELETE /memory/{id}).

        Sets status=invalidated + revoked_at + deleted_by_user_id; the physical row is
        retained (never hard-deleted) per the MEM02 D9 full-retention model and the MEM03
        transparency vision. ``reason`` is written to metadata_json.invalidation_reason.

        Errors pass through: CX_ERR_MEM03_MEMORY_NOT_FOUND / CX_ERR_MEM03_USER_MISMATCH /
        CX_ERR_MEM03_ALREADY_INVALIDATED / CX_ERR_MEM03_INVALIDATE_FAILED / CX_ERR_MEM03_QUOTA.
        """
        params: dict = {}
        if namespace:
            params["namespace"] = namespace
        if reason:
            params["reason"] = reason
        return request("DELETE", f"/memory/{memory_id}", params=params or None, timeout=10.0)
