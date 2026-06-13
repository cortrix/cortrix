"""SQL resource tests (POST /sql, real-arch wire)."""

from __future__ import annotations

import json

import httpx
import pytest
import respx

from cortrix import Cortrix, FeatureNotAvailableError
from cortrix.types import SqlResult

_OK = {"sql": "SELECT 1", "results": [{"n": 1}], "row_count": 1, "execution_time_ms": 5}


@respx.mock
def test_query_basic(api_base: str, client: Cortrix) -> None:
    route = respx.post(api_base + "/sql").mock(return_value=httpx.Response(200, json=_OK))
    out = client.sql.query("salesdb", question="top 5 sales this month", max_rows=5, explain=True)
    assert isinstance(out, SqlResult) and out.row_count == 1
    body = json.loads(route.calls.last.request.content)
    assert body["question"] == "top 5 sales this month"
    assert body["max_rows"] == 5 and body["explain"] is True
    assert body["namespace"] == "salesdb"  # forward-compat extra (flagged)


@respx.mock
def test_query_ce_feature_not_available(api_base: str, client: Cortrix) -> None:
    respx.post(api_base + "/sql").mock(
        return_value=httpx.Response(
            403,
            json={"error": {"code": "CX_ERR_FEATURE_NOT_AVAILABLE",
                            "message": "Feature not available", "retryable": False, "category": "permanent"}},
        )
    )
    with pytest.raises(FeatureNotAvailableError):
        client.sql.query(question="q")
