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

from . import __version__
from .mcp_server import CortrixMCPServer
from .tools import register_all
from .transport import CORTRIX_NAMESPACE

# MCP clients show this to the agent at the start of every session; it is how the
# agent learns to search before it reads. Claude Code keeps only the first 2,048
# characters of a server's instructions, so tests hold it under that.
INSTRUCTIONS = (
    "Cortrix is a semantic store for documents and agent memory. Documents are "
    "indexed in namespaces; cortrix_query searches the "
    f'"{CORTRIX_NAMESPACE}" namespace unless you pass `namespaces`.\n\n'
    "When a question may be answered by the indexed documents, search before you read:\n"
    "1. Call cortrix_query with the question, or with the names, terms and IDs you "
    "need. Each hit has `content` (the matching chunk), a score, and `metadata` with "
    "the document's `doc_id`, `filename` and any fields set at upload, such as a "
    "source path.\n"
    "2. Read only what the hits leave open. If you can open the original files, go to "
    "the ones the hits point to instead of reading files up front; otherwise query "
    "again with narrower terms.\n"
    "3. Cite from the hit: the identifiers in `metadata` come from the index, so there "
    "is no need to look them up again.\n"
    "4. To see what is indexed, call cortrix_list_namespaces, then "
    "cortrix_list_documents for one namespace.\n\n"
    "Tools that upload documents, add watchers or change memory write to the store; "
    "call them only when the user asks for that.\n\n"
    "Every tool returns a GEN-Agent envelope: {data, meta:{retryable, category, "
    "retry_after_ms, structured_data}}; tool failures return a model-visible "
    "structured error result with the same retry and correlation fields."
)

mcp = CortrixMCPServer("cortrix", version=__version__, instructions=INSTRUCTIONS)

# Register all tools at import time so test suites and the stdio runner share one server.
register_all(mcp)


def main() -> None:
    """Console-script entry point (pyproject [project.scripts] cortrix-mcp)."""
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
