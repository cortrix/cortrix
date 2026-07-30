"""End-to-end SDK v2 dual-era tests over real stdio subprocesses."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

import anyio
import mcp.types as types
import pytest
from mcp import Client, MCPError, StdioServerParameters
from mcp.client.stdio import stdio_client
from cortrix_mcp import __version__

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
HARNESS = Path(__file__).with_name("stdio_test_server.py")
BASELINE_MANIFEST_SHA256 = "fb2296f6840d3c2bf256430cdfbbaed7e520326aac6fbcdcd0de33104156b5f0"


def _parameters(*, harness: bool = False, scenario: str = "success") -> StdioServerParameters:
    args = [str(HARNESS)] if harness else ["-m", "cortrix_mcp.server"]
    return StdioServerParameters(
        command=sys.executable,
        args=args,
        cwd=PACKAGE_ROOT,
        env={
            "CORTRIX_MCP_ADMIN": "false",
            "CORTRIX_MCP_TEST_SCENARIO": scenario,
            "CORTRIX_API_KEY": "stdio-secret-value",
        },
    )


def _manifest_hash(tools: list[types.Tool]) -> str:
    manifest = [
        {
            "name": tool.name,
            "description": tool.description,
            "inputSchema": tool.input_schema,
        }
        for tool in sorted(tools, key=lambda item: item.name)
    ]
    canonical = json.dumps(
        manifest,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode()
    return hashlib.sha256(canonical).hexdigest()


@pytest.mark.parametrize(
    ("mode", "expected_protocol"),
    [("auto", "2026-07-28"), ("legacy", "2025-11-25")],
)
def test_real_stdio_dual_era_surface_and_admin_denial(mode, expected_protocol):
    async def exercise() -> None:
        async with Client(stdio_client(_parameters()), mode=mode) as client:
            listed = await client.list_tools()
            denied = await client.call_tool(
                "cortrix_admin_db_import_run",
                {"connection_ref": "test", "namespace": "test", "table": "items"},
            )
            assert client.protocol_version == expected_protocol
            assert client.server_info is not None
            assert client.server_info.name == "cortrix"
            assert client.server_info.version == __version__
            assert len(listed.tools) == 31
            assert _manifest_hash(listed.tools) == BASELINE_MANIFEST_SHA256
            assert denied.is_error is True
            assert denied.structured_content["code"] == "CX_ERR_MCP_ADMIN_REQUIRED"
            assert denied.structured_content["category"] == "auth"

    anyio.run(exercise)


@pytest.mark.parametrize("mode", ["auto", "legacy"])
def test_real_stdio_representative_success_and_identity(mode):
    async def exercise() -> None:
        async with Client(stdio_client(_parameters(harness=True)), mode=mode) as client:
            first_health = await client.call_tool("cortrix_health", {})
            second_health = await client.call_tool("cortrix_health", {})
            query = await client.call_tool("cortrix_query", {"query": "stdio"})
            memory = await client.call_tool("cortrix_memory_search", {"query": "remember"})
            task = await client.call_tool("cortrix_task_status", {"task_id": "task-1"})

            assert not any(
                result.is_error
                for result in (first_health, second_health, query, memory, task)
            )
            first = json.loads(first_health.content[0].text)
            second = json.loads(second_health.content[0].text)
            first_identity = first["data"]["observed_identity"]
            second_identity = second["data"]["observed_identity"]
            first_structured = first["meta"]["structured_data"]
            second_structured = second["meta"]["structured_data"]

            assert first_identity["X-Session-Id"] == second_identity["X-Session-Id"]
            assert first_identity["X-Trace-Id"] != second_identity["X-Trace-Id"]
            assert first_identity["X-Agent-Id"] == second_identity["X-Agent-Id"] == "cortrix-mcp"
            assert first_structured["session_id"] == first_identity["X-Session-Id"]
            assert first_structured["trace_id"] == first_identity["X-Trace-Id"]
            assert second_structured["trace_id"] == second_identity["X-Trace-Id"]
            assert first["data"]["authorization_present"] is True

            rendered = json.dumps(
                [
                    result.model_dump(by_alias=True, exclude_none=True)
                    for result in (first_health, second_health, query, memory, task)
                ]
            )
            assert "stdio-secret-value" not in rendered

    anyio.run(exercise)


@pytest.mark.parametrize(
    ("scenario", "code", "category", "retryable"),
    [
        ("timeout", "CX_ERR_MCP_BACKEND_TIMEOUT", "transient", True),
        ("connect", "CX_ERR_MCP_BACKEND_UNAVAILABLE", "transient", True),
        ("4xx", "CX_ERR_NS_UNAUTHORIZED", "auth", False),
        ("5xx", "CX_ERR_BACKEND_BUSY", "transient", True),
    ],
)
def test_real_stdio_model_visible_backend_errors(scenario, code, category, retryable):
    async def exercise() -> None:
        async with Client(
            stdio_client(_parameters(harness=True, scenario=scenario)),
            mode="auto",
        ) as client:
            result = await client.call_tool("cortrix_health", {})
            assert result.is_error is True
            assert result.structured_content["code"] == code
            assert result.structured_content["category"] == category
            assert result.structured_content["retryable"] is retryable
            assert code in result.content[0].text
            assert "stdio-secret-value" not in json.dumps(
                result.model_dump(by_alias=True, exclude_none=True)
            )

    anyio.run(exercise)


def test_real_stdio_schema_error_is_model_visible():
    async def exercise() -> None:
        async with Client(stdio_client(_parameters()), mode="auto") as client:
            result = await client.call_tool("cortrix_query", {})
            assert result.is_error is True
            assert result.structured_content == {
                "code": "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL",
                "retryable": False,
                "category": "permanent",
                "retry_after_ms": None,
                "structured_data": {"tool": "cortrix_query"},
            }

    anyio.run(exercise)


def test_real_stdio_unknown_tool_is_model_visible():
    async def exercise() -> None:
        async with Client(stdio_client(_parameters()), mode="auto") as client:
            result = await client.call_tool("cortrix_unknown_tool", {})
            assert result.is_error is True
            assert result.structured_content == {
                "code": "CX_ERR_MCP_TOOL_NOT_FOUND",
                "retryable": False,
                "category": "permanent",
                "retry_after_ms": None,
                "structured_data": {"tool": "cortrix_unknown_tool"},
            }

    anyio.run(exercise)


def test_real_stdio_unknown_protocol_method_is_host_visible():
    async def exercise() -> None:
        async with Client(stdio_client(_parameters()), mode="auto") as client:
            with pytest.raises(MCPError) as error:
                await client.session.send_request(
                    types.Request(
                        method="cortrix/unknown-protocol-method",
                        params={},
                    ),
                    types.EmptyResult,
                )
            assert error.value.code == types.METHOD_NOT_FOUND

    anyio.run(exercise)
