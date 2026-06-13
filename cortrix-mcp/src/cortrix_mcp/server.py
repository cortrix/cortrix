"""Cortrix MCP Server — FastMCP entry point.

Registers all 29 tools + 2 admin tools and runs in stdio mode for IDE agents
(Claude Code / Cline / Cursor). Migrated and expanded from the MVP single-file
mcp-server/cortrix_mcp_server.py.

Usage:
    cortrix-mcp                          # stdio mode (installed entry point)
    python -m cortrix_mcp.server         # stdio mode (module)
    CORTRIX_URL=http://127.0.0.1:8080 cortrix-mcp

Environment variables (feature design section 5.3):
    CORTRIX_URL         Cortrix backend URL                 (default http://127.0.0.1:8080)
    CORTRIX_NAMESPACE   default namespace                   (default "default")
    CORTRIX_API_KEY     Bearer token (P08 API key)          (default empty)
    CORTRIX_MCP_ADMIN   admin override for F16a admin tools (default false)
    CORTRIX_MCP_TIMEOUT HTTP timeout in seconds             (default 30)
"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from .tools import register_all

mcp = FastMCP(
    "cortrix",
    instructions=(
        "Cortrix semantic storage — document indexing, hybrid semantic search "
        "(vector + BM25), conversation memory (MEM02/03/04/05), async ingestion, "
        "directory watchers, operation audit, and admin DB import. Every tool returns "
        "a GEN-Agent envelope: {data, meta:{retryable, category, retry_after_ms, "
        "structured_data}}; errors raise McpError carrying the same 4 fields."
    ),
)

# Register all tools at import time so test suites and the stdio runner share one server.
register_all(mcp)


def main() -> None:
    """Console-script entry point (pyproject [project.scripts] cortrix-mcp)."""
    mcp.run()


if __name__ == "__main__":
    main()
