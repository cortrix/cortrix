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


# The tools that leave the store unchanged. In read-only mode only these are registered.
READ_ONLY_TOOLS = frozenset({
    "cortrix_health", "cortrix_query", "cortrix_list_documents", "cortrix_list_namespaces",
    "cortrix_memory_search", "cortrix_list_interactions", "cortrix_document_status",
    "cortrix_list_watchers", "cortrix_cross_ns_query", "cortrix_memory_search_filter",
    "cortrix_memory_get_audit", "cortrix_list_operations", "cortrix_memory_list",
    "cortrix_task_status", "cortrix_query_explain", "cortrix_find_documents",
})


class CortrixMCPServer(MCPServer):
    """MCPServer that preserves the Cortrix model-visible error contract.

    With ``read_only=True`` the server registers only the tools in ``READ_ONLY_TOOLS``;
    the others are never advertised to the client, so an agent in a question-answering
    session cannot upload documents or change namespaces, watchers, interactions,
    tasks, memory or database imports.
    """

    def __init__(self, *args: Any, read_only: bool = False, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.read_only = read_only

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
        """Register a tool and translate internal failures at the tool boundary.

        In read-only mode a tool outside ``READ_ONLY_TOOLS`` is returned unregistered.
        """
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
            if self.read_only and (name or function.__name__) not in READ_ONLY_TOOLS:
                return function
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
