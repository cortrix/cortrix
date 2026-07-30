"""Cortrix MCP Server — SDK v2 dual-era stdio entry point.

Registers all 29 tools + 2 admin tools and runs in stdio mode for IDE agents
(Claude Code / Cline / Cursor). The official ``MCPServer`` negotiates modern
``2026-07-28`` through ``server/discover`` and retains the legacy
``2025-11-25`` initialize path on the same server process.

Usage:
    cortrix-mcp                          # stdio mode (installed entry point)
    python -m cortrix_mcp.server         # stdio mode (module)
    CORTRIX_URL=http://127.0.0.1:8420 cortrix-mcp

Environment variables (feature design section 5.3):
    CORTRIX_URL         Cortrix backend URL                 (default http://127.0.0.1:8420)
    CORTRIX_NAMESPACE   default namespace                   (default "default")
    CORTRIX_API_KEY     Bearer token (P08 API key)          (default empty)
    CORTRIX_MCP_ADMIN   admin override for F16a admin tools (default false)
    CORTRIX_MCP_TIMEOUT HTTP timeout in seconds             (default 30)
"""

from __future__ import annotations

from .mcp_server import CortrixMCPServer
from .tools import register_all

mcp = CortrixMCPServer(
    "cortrix",
    instructions=(
        "Cortrix semantic storage — document indexing, hybrid semantic search "
        "(vector + BM25), conversation memory (MEM02/03/04/05), async ingestion, "
        "directory watchers, operation audit, and admin DB import. Every tool returns "
        "a GEN-Agent envelope: {data, meta:{retryable, category, retry_after_ms, "
        "structured_data}}; tool failures return a model-visible structured error "
        "result with the same retry and correlation fields."
    ),
)

# Register all tools at import time so test suites and the stdio runner share one server.
register_all(mcp)


def main() -> None:
    """Console-script entry point (pyproject [project.scripts] cortrix-mcp)."""
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
