"""Cortrix-specific behavior layered on the official SDK v2 ``MCPServer``."""

from __future__ import annotations

import inspect
from functools import wraps
from typing import Any, Callable, Optional

from mcp import MCPError
from mcp.server.mcpserver import MCPServer
from mcp.types import CallToolResult, ToolAnnotations

from .errors import (
    CortrixToolError,
    internal_tool_error,
    tool_error,
    tool_error_result,
)


def _caused_by_validation_error(error: BaseException) -> bool:
    """Detect SDK argument validation without exposing rejected input values."""
    current: Optional[BaseException] = error
    while current is not None:
        if current.__class__.__name__ == "ValidationError":
            return True
        current = current.__cause__
    return False


class CortrixMCPServer(MCPServer):
    """MCPServer that preserves the Cortrix model-visible error contract."""

    def tool(
        self,
        name: str | None = None,
        title: str | None = None,
        description: str | None = None,
        annotations: ToolAnnotations | None = None,
        icons: list[Any] | None = None,
        meta: dict[str, Any] | None = None,
        structured_output: bool | None = None,
    ) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
        """Register a tool and translate internal failures at the tool boundary."""
        register = super().tool(
            name=name,
            title=title,
            description=description,
            annotations=annotations,
            icons=icons,
            meta=meta,
            # Preserve the v1 success wire shape: JSON text content without a
            # newly inferred outputSchema or structuredContent field.
            structured_output=False if structured_output is None else structured_output,
        )

        def decorator(function: Callable[..., Any]) -> Callable[..., Any]:
            if inspect.iscoroutinefunction(function):

                @wraps(function)
                async def async_wrapper(*args: Any, **kwargs: Any) -> Any:
                    try:
                        return await function(*args, **kwargs)
                    except CortrixToolError as error:
                        return tool_error_result(error)

                wrapped = async_wrapper
            else:

                @wraps(function)
                def sync_wrapper(*args: Any, **kwargs: Any) -> Any:
                    try:
                        return function(*args, **kwargs)
                    except CortrixToolError as error:
                        return tool_error_result(error)

                wrapped = sync_wrapper

            return register(wrapped)

        return decorator

    async def call_tool(
        self,
        name: str,
        arguments: dict[str, Any],
        context: Any = None,
    ) -> CallToolResult:
        """Call a tool while keeping tool and protocol failures in separate layers."""
        try:
            return await super().call_tool(name, arguments, context)
        except MCPError:
            # SDK protocol errors must remain host-visible JSON-RPC failures.
            raise
        except Exception as error:
            message = str(error)
            if "Unknown tool:" in message:
                failure = tool_error(
                    "CX_ERR_MCP_TOOL_NOT_FOUND",
                    structured_data={"tool": name},
                )
            elif _caused_by_validation_error(error):
                failure = tool_error(
                    "CX_ERR_MCP_SCHEMA_VALIDATION_FAIL",
                    structured_data={"tool": name},
                )
            else:
                failure = internal_tool_error()
            return tool_error_result(failure)
