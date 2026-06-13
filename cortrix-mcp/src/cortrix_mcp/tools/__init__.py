"""Tool modules grouped by origin (feature design section 8.1).

  core      — MVP 12 tools (migrated + GEN-Agent 4-field adaptation)
  extended  — 4 extended tools (cross-NS query / async upload / memory filter / extract trigger)
  new       — 4 new tools (memory_extract / task_status / cancel_task / query_explain)
  memory    — MEM02 +2 / MEM04 +1 / MEM03 +4 + batch_submit + list_operations
  admin     — F16a admin scope 2 tools

``register_all(mcp)`` wires every tool onto the FastMCP instance.
"""

from . import admin, core, extended, memory, new


def register_all(mcp) -> None:
    """Register all 29 tools + 2 admin tools onto the FastMCP server."""
    core.register(mcp)
    extended.register(mcp)
    new.register(mcp)
    memory.register(mcp)
    admin.register(mcp)
