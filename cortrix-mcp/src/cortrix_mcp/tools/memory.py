"""Memory & operations tools: #21-#29 (feature design sections 4.4 / 4.5 / 4.5.bis /
4.5.ter / 4.5.quinquies).

  #21 cortrix_memory_get_audit  -> GET  /operations?action_in=memory_* (memory extraction audit over operation_log)
  #22 cortrix_memory_revoke_fact-> POST /memory/{id}/revoke      (memory extraction self-revoke)             [integration]
  #23 cortrix_memory_opt_out    -> POST /memory/session/{id}/opt-out[/revoke] (memory opt-out)
  #24 cortrix_batch_submit      -> POST /documents/batch         (batch submit, <=100 docs, async)
  #25 cortrix_list_operations   -> GET  /operations              (operation_log)
  #26 cortrix_memory_list       -> GET  /memory                  (memory transparency endpoint 1, API spec real)
  #27 cortrix_memory_create     -> POST /memory                  (memory transparency endpoint 2, API spec real)
  #28 cortrix_memory_edit       -> PATCH /memory/{id}            (memory transparency endpoint 3, API spec real)
  #29 cortrix_memory_invalidate -> DELETE /memory/{id}           (memory transparency endpoint 4 soft-delete, API spec real)

[integration] = endpoint not yet present in the backend HTTP surface; implemented per the feature
design contract and exercised with mocked HTTP responses during standalone development.
(#22 stays [integration]: the revoke HTTP route is deferred — POST /memory/invalidations/{id}/revoke
currently returns 501 pending Wave S2 wiring.)
"""

from __future__ import annotations

from typing import Optional

from ..transport import CORTRIX_NAMESPACE, request


def register(mcp) -> None:
    # ---- memory extraction reverse +2 ---------------------------------------
    @mcp.tool()
    def cortrix_memory_get_audit(
        memory_id: Optional[str] = None,
        namespace: str = "",
        limit: int = 50,
    ) -> dict:
        """Query the memory audit log (memory extraction; reuses the operation_log).

        There is no dedicated /memory/audit endpoint; the audit trail lives in the operation log
        operation_log. This maps to GET /operations filtered to the memory lifecycle
        actions (memory_extract, memory_invalidate, memory_revoke) — the canonical pointer
        the backend itself returns from /memory/invalidations.

        Args:
            memory_id: optional. The operation_log query surface has no per-memory_id
                filter (OperationLogFilter exposes action/namespace_id/user_id/
                resource_type/trace_id/time-range only), so this is applied client-side to
                the returned rows' resource_id, not pushed to the backend.
            namespace: target namespace (uses default if empty); maps to namespace_id.
            limit: max entries to return (default 50, backend cap 200).
        """
        ns = namespace or CORTRIX_NAMESPACE
        params: dict = {
            "action_in": "memory_extract,memory_invalidate,memory_revoke",
            "namespace_id": ns,
            "limit": min(limit, 200),
        }
        result = request("GET", "/operations", params=params, data_key="operations", timeout=10.0)
        if memory_id:
            entries = result.get("data") or []
            if isinstance(entries, list):
                result["data"] = [e for e in entries if e.get("resource_id") == memory_id]
        return result

    @mcp.tool()
    def cortrix_memory_revoke_fact(
        memory_id: str,
        namespace: str = "",
    ) -> dict:
        """Self-service revoke of an auto-extracted fact (memory extraction).

        Marks the fact auto_revoke_eligible; the physical row is retained (full-retention
        model). Integration deferred: POST /memory/{id}/revoke not yet in api/paths/*.yaml — mock.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body = {"namespace": ns}
        return request(
            "POST", f"/memory/invalidations/{memory_id}/revoke", json_body=body, timeout=10.0
        )

    # ---- memory opt-out reverse +1 ------------------------------------------
    @mcp.tool()
    def cortrix_memory_opt_out(
        session_id: str,
        opt_out: bool = True,
        namespace: str = "",
        reason: str = "",
    ) -> dict:
        """Per-session memory opt-out (and opt-out revoke), memory opt-out + combined.

        Args:
            session_id: session to opt out of memory extraction.
            opt_out: True to opt out, False to revoke the opt-out (admin scope).
            namespace: target namespace (uses default if empty).
            reason: optional free-text reason recorded with the opt-out / revoke.

        Routes to the backend's memory opt-out surface (memory_routes.cpp M5): the session id is
        carried in the URL path, not the body. opt_out=True  -> POST
        /memory/session/{id}/opt-out (write scope); opt_out=False -> POST
        /memory/session/{id}/opt-out/revoke (admin scope).
        """
        ns = namespace or CORTRIX_NAMESPACE
        body: dict = {"namespace": ns}
        if reason:
            body["reason"] = reason
        suffix = "/opt-out" if opt_out else "/opt-out/revoke"
        return request("POST", f"/memory/session/{session_id}{suffix}", json_body=body, timeout=10.0)

    # ---- Batch submit reverse +1 --------------------------------------------
    @mcp.tool()
    def cortrix_batch_submit(
        namespace: str,
        documents: list[dict],
        async_: bool = True,
        on_duplicate: str = "skip",
    ) -> dict:
        """Submit up to 100 documents to a namespace in one batch.

        Args:
            namespace: target namespace.
            documents: up to 100 items, each {doc_id, content, metadata?}.
            async_: must be True in V1 (topic 3).
            on_duplicate: skip / overwrite / error (topic 2).

        Returns a results list (same order as documents) + structured_data
        (succeeded / failed / total / coverage_ratio). A batch-level failure returns the
        GEN-Agent error response. Backed by the live POST /documents/batch route.
        """
        body = {
            "namespace": namespace,
            "documents": documents,
            "options": {
                "async": async_,
                "on_duplicate": on_duplicate,
            },
        }
        return request("POST", "/documents/batch", json_body=body, timeout=120.0)

    # ---- operation log reverse +1 -------------------------------------------
    @mcp.tool()
    def cortrix_list_operations(
        user_id: Optional[str] = None,
        namespace: Optional[str] = None,
        action: Optional[str] = None,
        action_in: Optional[list[str]] = None,
        start_time: Optional[int] = None,
        end_time: Optional[int] = None,
        limit: int = 50,
        offset: int = 0,
        sort_order: str = "DESC",
    ) -> dict:
        """Query the operation log (operation log; CE 30-day retention + 100K cap; GET /operations).

        Args:
            user_id: defaults to the current agent user_id when omitted (cross-user needs
                an admin key, else CX_ERR_OPLOG_UNAUTHORIZED).
            namespace: filter by namespace (sent as namespace_id).
            action: single operation log action enum value.
            action_in: multi-select list of action enum values (joined as a CSV action_in).
            start_time / end_time: Unix-millisecond time range (sent as from_timestamp /
                to_timestamp).
            limit: max rows (default 50, backend cap 200).
            offset: pagination offset (the backend paginates by integer offset, not a cursor).
            sort_order: "DESC" (default) or "ASC".

        Returns operations (op_id / user_id / action / namespace / details_json / trace_id /
        session_id / created_at) + structured_data (total / has_more / next_offset).
        Backed by the live GET /operations route (operations_routes.cpp).
        """
        params: dict = {"limit": min(limit, 200), "offset": offset, "sort_order": sort_order}
        if action_in:
            params["action_in"] = ",".join(action_in)
        for key, value in (
            ("user_id", user_id),
            ("namespace_id", namespace),
            ("action", action),
            ("from_timestamp", start_time),
            ("to_timestamp", end_time),
        ):
            if value is not None:
                params[key] = value
        return request("GET", "/operations", params=params, data_key="operations", timeout=10.0)

    # ---- memory transparency reverse +4 (real API spec endpoints) -----------
    @mcp.tool()
    def cortrix_memory_list(
        namespace: Optional[str] = None,
        memory_type: Optional[str] = None,
        include_invalidated: bool = False,
        limit: int = 50,
        offset: int = 0,
    ) -> dict:
        """List the current user_id's memories (memory transparency endpoint 1; GET /memory).

        Args:
            namespace: defaults to the current user_id's namespace when omitted.
            memory_type: filter by type (fact / preference / event).
            include_invalidated: include soft-deleted (status=invalidated) memories for
                transparency queries.
            limit: max rows (default 50, max 200).
            offset: pagination offset.

        Errors pass through: CX_ERR_MEMORY_USER_MISMATCH / CX_ERR_MEMORY_INVALID_TYPE /
        CX_ERR_MEMORY_QUOTA / CX_ERR_MEMORY_LIST_FAILED.
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
        """Create a memory (memory transparency endpoint 2; POST /memory).

        The POST path does not pass through LLM quality filtering (memory transparency lock);
        extraction_method is set to 'user_create' server-side.

        Errors pass through: CX_ERR_MEMORY_USER_MISMATCH / CX_ERR_MEMORY_INVALID_TYPE /
        CX_ERR_MEMORY_CONTENT_TOO_LONG / CX_ERR_MEMORY_QUOTA / CX_ERR_MEMORY_CREATE_FAILED.
        """
        body: dict = {"namespace": namespace, "content": content, "memory_type": memory_type}
        if metadata:
            body["metadata"] = metadata
        return request("POST", "/memory", json_body=body, timeout=10.0)

    @mcp.tool()
    def cortrix_memory_edit(
        memory_id: str,
        namespace: str = "",
        content: Optional[str] = None,
        metadata: Optional[dict] = None,
    ) -> dict:
        """Edit an existing memory's content / metadata (memory transparency endpoint 3; PATCH /memory/{id}).

        metadata merge semantics (not replace); extraction_method is overwritten to
        'user_edit' server-side. Semantically a new user_edit memory + invalidation of the
        old one, hence PATCH not PUT.

        Errors pass through: CX_ERR_MEMORY_NOT_FOUND / CX_ERR_MEMORY_USER_MISMATCH /
        CX_ERR_MEMORY_ALREADY_INVALIDATED / CX_ERR_MEMORY_CONTENT_TOO_LONG /
        CX_ERR_MEMORY_EDIT_FAILED / CX_ERR_MEMORY_QUOTA.
        """
        ns = namespace or CORTRIX_NAMESPACE
        body: dict = {"namespace": ns}
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
        """Soft-delete a memory (memory transparency endpoint 4; DELETE /memory/{id}).

        Sets status=invalidated + revoked_at + deleted_by_user_id; the physical row is
        retained (never hard-deleted) per the memory extraction full-retention model and the memory transparency
        transparency vision. ``reason`` is written to metadata_json.invalidation_reason.

        Errors pass through: CX_ERR_MEMORY_NOT_FOUND / CX_ERR_MEMORY_USER_MISMATCH /
        CX_ERR_MEMORY_ALREADY_INVALIDATED / CX_ERR_MEMORY_INVALIDATE_FAILED / CX_ERR_MEMORY_QUOTA.
        """
        params: dict = {}
        if namespace:
            params["namespace"] = namespace
        if reason:
            params["reason"] = reason
        return request("DELETE", f"/memory/{memory_id}", params=params or None, timeout=10.0)
