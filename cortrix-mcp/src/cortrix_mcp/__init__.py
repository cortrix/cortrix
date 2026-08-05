"""Cortrix MCP Server — expose the Cortrix HTTP API as MCP tools for IDE agents.

This package wraps the frozen API spec OpenAPI surface (cortrix-server /api/v1/*) as
29 MCP tools + 2 admin tools, with the GEN-Agent two-layer 4-field schema
(retryable / category / retry_after_ms / structured_data) on every response.

Distribution: pip (`cortrix-mcp`) + Docker (`cortrix/mcp`).
Transport to backend: httpx direct HTTP (no Python SDK), prefix `{CORTRIX_URL}/api/v1/<path>`.
"""

__version__ = "1.0.0rc1"
