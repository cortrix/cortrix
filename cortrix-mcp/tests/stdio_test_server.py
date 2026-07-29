"""Real stdio test entry point with an in-process fake backend client.

This keeps the MCP transport and Cortrix adapter real while avoiding a new
network listener or mutations to a live Cortrix backend.
"""

from __future__ import annotations

import os
from typing import Any

import httpx

from cortrix_mcp import transport
from cortrix_mcp.server import mcp


class FakeBackendClient:
    """Return deterministic backend responses and capture identity headers."""

    def request(
        self,
        method: str,
        url: str,
        *,
        json: dict[str, Any] | None = None,
        params: dict[str, Any] | None = None,
        headers: dict[str, str] | None = None,
        timeout: float | None = None,
    ) -> httpx.Response:
        del json, params, timeout
        scenario = os.environ.get("CORTRIX_MCP_TEST_SCENARIO", "success")
        request = httpx.Request(method, url)

        if scenario == "timeout":
            raise httpx.TimeoutException("stdio harness timeout", request=request)
        if scenario == "connect":
            raise httpx.ConnectError("stdio harness unavailable", request=request)
        if scenario == "4xx":
            return httpx.Response(
                403,
                json={
                    "error": {
                        "code": "CX_ERR_NS_UNAUTHORIZED",
                        "message": "Namespace access denied",
                        "retryable": False,
                        "category": "auth",
                        "retry_after_ms": None,
                        "structured_data": {"namespace": "denied"},
                    }
                },
                request=request,
            )
        if scenario == "5xx":
            return httpx.Response(
                503,
                json={
                    "error": {
                        "code": "CX_ERR_BACKEND_BUSY",
                        "message": "Backend busy",
                        "retryable": True,
                        "category": "transient",
                        "retry_after_ms": 250,
                        "structured_data": {},
                    }
                },
                request=request,
            )

        sent = headers or {}
        observed_identity = {
            key: sent.get(key)
            for key in ("X-Session-Id", "X-Trace-Id", "X-Agent-Id")
        }
        if url.endswith("/system/health/live"):
            body: dict[str, Any] = {
                "status": "ok",
                "observed_identity": observed_identity,
                "authorization_present": "Authorization" in sent,
            }
        elif url.endswith("/query"):
            body = {
                "data": [{"id": "doc-1", "score": 0.9}],
                "meta": {"coverage_ratio": 1.0, "namespaces_failed": []},
            }
        elif url.endswith("/memory/search"):
            body = {"memories": [{"memory_id": "memory-1"}]}
        elif url.endswith("/documents/tasks/task-1/progress"):
            body = {"task_id": "task-1", "status": "running", "progress": 0.5}
        else:
            body = {"status": "ok"}

        return httpx.Response(
            200,
            json=body,
            headers={"X-Trace-Id": sent.get("X-Trace-Id", "")},
            request=request,
        )


transport._client = FakeBackendClient()
mcp.run(transport="stdio")
