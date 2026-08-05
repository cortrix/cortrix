"""Cortrix Agent core (agent chat mode).

UI-agnostic kernel for the Cortrix built-in Agent. Phase 1 V1.0 ships the
``ChatExecutor`` (fixed RAG flow, no autonomous tool-use); V1.5 adds
``ToolUseExecutor`` and V2 adds ``PlanExecuteExecutor`` behind the shared
``IAgentExecutor`` interface.

Public surface::

    from agent_core import (
        IAgentExecutor, ChatExecutor, ChatContext,
        SessionStore, build_chat_prompt, scan_injection_keywords,
        AgentError, agent_error_response, ERROR_TABLE,
        build_explain_meta,
    )
"""

from __future__ import annotations

from .errors import (
    ERROR_TABLE,
    STARTUP_ERROR_TABLE,
    AgentError,
    agent_error_response,
)
from .executor import ChatContext, ChatExecutor, IAgentExecutor, ToolUseExecutor
from .explain import build_response_meta
from .mem_coprocess import MemoryCoprocessor
from .prompt import build_chat_prompt, scan_injection_keywords
from .session_store import SessionStore, WINDOW_SIZE

__all__ = [
    "IAgentExecutor",
    "ChatExecutor",
    "ToolUseExecutor",
    "ChatContext",
    "SessionStore",
    "WINDOW_SIZE",
    "build_chat_prompt",
    "scan_injection_keywords",
    "AgentError",
    "agent_error_response",
    "ERROR_TABLE",
    "STARTUP_ERROR_TABLE",
    "build_response_meta",
    "MemoryCoprocessor",
]
