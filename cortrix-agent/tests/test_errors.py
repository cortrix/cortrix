"""Unit tests for the GEN-Agent 4-field error envelope (design section 9.3 / 10)."""

from __future__ import annotations

import pytest

from agent_core.errors import (
    ERROR_TABLE,
    STARTUP_ERROR_TABLE,
    AgentError,
    agent_error_response,
)


def test_error_table_has_seven_runtime_codes():
    """Section 10.1: exactly 7 V1.0-enabled runtime chat-path codes."""
    assert len(ERROR_TABLE) == 7
    assert "CX_ERR_F48_RAG_FAILED" in ERROR_TABLE
    assert "CX_ERR_F48_SESSION_NOT_FOUND" in ERROR_TABLE
    assert "CX_ERR_F48_INTERACTION_LOG_FAILED" in ERROR_TABLE


def test_startup_table_has_five_codes_independent_group():
    """Section 10.3: 5 startup/init codes, a separate group from the runtime 7."""
    assert len(STARTUP_ERROR_TABLE) == 5
    assert "CX_ERR_F48_CORTRIX_SERVER_UNREACHABLE" in STARTUP_ERROR_TABLE
    # The two tables must not overlap.
    assert set(ERROR_TABLE) & set(STARTUP_ERROR_TABLE) == set()


def test_to_dict_emits_exactly_the_four_plus_two_fields():
    """The envelope is {error:{code, message, retryable, category, retry_after_ms,
    structured_data}} (design section 9.3)."""
    err = AgentError("CX_ERR_F48_RAG_FAILED", structured_data={"cortrix_server_error": "boom"})
    payload = err.to_dict()
    assert set(payload.keys()) == {"error"}
    body = payload["error"]
    assert set(body.keys()) == {
        "code",
        "message",
        "retryable",
        "category",
        "retry_after_ms",
        "structured_data",
    }
    assert body["code"] == "CX_ERR_F48_RAG_FAILED"
    assert body["structured_data"] == {"cortrix_server_error": "boom"}


def test_known_code_defaults_category_retryable_and_status_from_registry():
    err = AgentError("CX_ERR_F48_LLM_TIMEOUT")
    assert err.category == "timeout"
    assert err.retryable is True
    assert err.http_status == 504
    assert err.message == "LLM provider request timed out"


def test_session_not_found_is_permanent_404():
    err = AgentError("CX_ERR_F48_SESSION_NOT_FOUND", structured_data={"session_id": "x"})
    assert err.category == "permanent"
    assert err.retryable is False
    assert err.http_status == 404


def test_caller_can_override_registry_defaults():
    err = AgentError(
        "CX_ERR_F48_RAG_FAILED",
        retryable=False,
        retry_after_ms=2000,
        message="custom",
    )
    assert err.retryable is False
    assert err.retry_after_ms == 2000
    assert err.message == "custom"


def test_unknown_code_falls_back_to_permanent_500():
    err = AgentError("CX_ERR_F48_TOTALLY_UNKNOWN")
    assert err.category == "permanent"
    assert err.retryable is False
    assert err.http_status == 500
    # message defaults to the code itself when not in the registry.
    assert err.message == "CX_ERR_F48_TOTALLY_UNKNOWN"


def test_agent_error_response_helper_matches_to_dict():
    direct = AgentError("CX_ERR_F48_LLM_QUOTA_EXCEEDED").to_dict()
    helper = agent_error_response("CX_ERR_F48_LLM_QUOTA_EXCEEDED")
    assert direct == helper


def test_agent_error_is_raisable():
    with pytest.raises(AgentError) as exc_info:
        raise AgentError("CX_ERR_F48_LLM_UNAVAILABLE")
    assert exc_info.value.code == "CX_ERR_F48_LLM_UNAVAILABLE"
