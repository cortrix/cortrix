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

# MCP clients show the instructions to the agent at the start of every session; they are
# how the agent learns to search before it reads. Claude Code keeps only the first 2,048
# characters of a server's instructions, so tests hold them under that.
_SEARCH_FIRST = (
    "When a question may be answered by the indexed documents, search before you read:\n"
    "1. Call cortrix_query with the question, or with the names, terms and IDs you "
    "need. Results are grouped by document: `doc_id`, `source_path` (the name it was "
    "indexed under), `metadata` with the fields set at upload, and `evidence`, which is "
    "the section around the best hit and the matching chunks of the others.\n"
    "2. Read only what the hits leave open. If you can open the original files, go to "
    "the ones the hits point to instead of reading files up front; otherwise query "
    "again with narrower terms.\n"
    "3. Cite from the hit: the identifiers in `metadata` come from the index, so there "
    "is no need to look them up again.\n"
    "4. To see what is indexed, call cortrix_list_namespaces, then "
    "cortrix_list_documents for one namespace.\n\n"
    "Only the tools that search, list or report status leave the store unchanged. "
    "The others upload documents or change namespaces, watchers, interactions, "
    "tasks, memory or database imports; call them only when the user asks for that.\n\n"
    "Every tool returns a GEN-Agent envelope: {data, meta:{retryable, category, "
    "retry_after_ms, structured_data}}; tool failures return a model-visible "
    "structured error result with the same retry and correlation fields."
)


def _namespace_guidance(namespace: str) -> str:
    if namespace == "*":
        # "*" fans every query out to every namespace, and a namespace that misses the
        # server's per-namespace deadline is dropped, so ask the agent to name one.
        return (
            "Documents are indexed in namespaces; cortrix_query searches every namespace "
            "unless you pass `namespaces`. A search of every namespace is slower, and a "
            "namespace that does not answer in time is left out of the results (it is "
            "listed in `namespaces_failed`), so pass the namespace that holds the "
            "documents you need: your project instructions may name it, or call "
            "cortrix_list_namespaces."
        )
    return (
        "Documents are indexed in namespaces; cortrix_query searches the "
        f'"{namespace}" namespace unless you pass `namespaces`.'
    )


def build_instructions(namespace: str) -> str:
    return (
        "Cortrix is a semantic store for documents and agent memory. "
        + _namespace_guidance(namespace)
        + "\n\n"
        + _SEARCH_FIRST
    )


INSTRUCTIONS = build_instructions(CORTRIX_NAMESPACE)

mcp = CortrixMCPServer("cortrix", version=__version__, instructions=INSTRUCTIONS)

# Register all tools at import time so test suites and the stdio runner share one server.
register_all(mcp)


def main() -> None:
    """Console-script entry point (pyproject [project.scripts] cortrix-mcp)."""
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
