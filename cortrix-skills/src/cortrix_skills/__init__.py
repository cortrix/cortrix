"""cortrix-skills — Cortrix Skill SDK (Agent framework adapter layer).

Third of the 3-path Agent access strategy (feature design section 1.1):

  * Python SDK   — Direct path (``cortrix.Cortrix``)
  * MCP Server   — IDE path (stdio MCP)
  * Skill SDK    — Framework path (this package)

Public surface::

    from cortrix_skills import CortrixToolKit
    from cortrix_skills.adapters import as_langchain_tools, as_claude_tools, as_openai_functions

``CortrixToolKit`` exposes 29 methods that 1:1 mirror the MCP 29 main tools
and call the Python SDK underneath (with a pass-through HTTP fallback for
endpoints the SDK does not yet expose). The adapters convert the toolkit into
LangChain Tools / Claude tool definitions / OpenAI function definitions.

The framework adapters are soft dependencies: importing ``cortrix_skills`` (or
``CortrixToolKit``) never requires langchain / anthropic / openai. Each
``as_*`` helper raises a clear ``ImportError`` only if its framework is missing
(``pip install cortrix-skills[langchain|claude|openai|all]``).

Error codes are passed through from the Python SDK unchanged (feature design section 7):
this package adds no ``CX_ERR_*`` prefix of its own.
"""

from __future__ import annotations

from .descriptor import ToolDescriptor, describe_method, json_schema_from_method
from .toolkit import TOOL_METHOD_NAMES, CortrixToolKit

__version__ = "1.0.0rc1"

__all__ = [
    "CortrixToolKit",
    "TOOL_METHOD_NAMES",
    "ToolDescriptor",
    "describe_method",
    "json_schema_from_method",
    "__version__",
]
