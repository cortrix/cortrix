"""Shared pytest fixtures + import bootstrap.

The package is laid out under ``src/`` (src-layout). We add ``src`` to ``sys.path``
directly rather than relying on the editable-install finder, which is more robust on
this workstation's filesystem.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
from typing import Any, Optional
from unittest.mock import MagicMock

import pytest

_SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

import cortrix_mcp.transport as transport  # noqa: E402
from cortrix_mcp.server import mcp  # noqa: E402


def call_tool_result(tool_name: str, **arguments):
    """Invoke a registered tool through the public MCPServer API."""
    return asyncio.run(mcp.call_tool(tool_name, arguments))


def get_tool_fn(name: str):
    """Return a compatibility caller backed by the public MCPServer API."""

    def invoke(**arguments):
        result = call_tool_result(name, **arguments)
        if result.is_error:
            raise AssertionError(f"{name} unexpectedly returned an error result: {result}")
        assert result.content and result.content[0].type == "text"
        return json.loads(result.content[0].text)

    return invoke


def make_response(
    *,
    status_code: int = 200,
    json_body: Optional[Any] = None,
    headers: Optional[dict] = None,
    raise_status: Optional[Exception] = None,
) -> MagicMock:
    """Build a fake httpx.Response good enough for transport.request()."""
    resp = MagicMock()
    resp.status_code = status_code
    body = json_body if json_body is not None else {}
    resp.json.return_value = body
    resp.content = json.dumps(body).encode() if body else b""
    resp.text = json.dumps(body) if body else ""
    resp.headers = headers or {"X-Trace-Id": "trace-test-1"}
    if raise_status is not None:
        resp.raise_for_status = MagicMock(side_effect=raise_status)
    else:
        resp.raise_for_status = MagicMock()
    return resp


@pytest.fixture
def mock_request(monkeypatch):
    """Patch the shared httpx client's .request and capture/inject responses.

    Usage::

        def test_x(mock_request):
            mock_request.set(json_body={"status": "ok"})
            out = get_tool_fn("cortrix_health")()
            assert mock_request.client.request.called
    """

    class _Mock:
        def __init__(self):
            self.client = MagicMock()
            self.client.request = MagicMock()

        def set(self, **kwargs):
            self.client.request.return_value = make_response(**kwargs)

        def set_side_effect(self, exc):
            self.client.request.side_effect = exc

        @property
        def last_call(self):
            return self.client.request.call_args

    m = _Mock()
    monkeypatch.setattr(transport, "_client", m.client)
    return m
