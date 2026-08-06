"""Public response / request types.

Wire-faithful dataclasses live in ``_generated.py`` (produced from the frozen
OpenAPI spec by ``scripts/generate_types.py``). This package re-exports them and
adds a few hand-written list/wrapper types plus Pythonic ergonomics
(``__iter__`` / ``__len__``) for endpoints whose JSON envelope the spec leaves
inline (e.g. document / namespace list responses).
"""

from __future__ import annotations

# Wire-faithful generated models (the authoritative shapes).
from ._generated import (
    AgentLlmConfig,
    AgentSession,
    ApiKey,
    ChatRequest,
    ChunkStrategy,
    Document,
    DocumentProgress,
    DocumentTask,
    GcStatus,
    LoginResponse,
    Memory,
    NamespaceCreateRequest,
    NsAclEntry,
    NsStats,
    Quota,
    QueryFilter,
    QueryMeta,
    QueryRequest,
    QueryResult,
    QueryResultItem,
    RegisterRequest,
    SqlRequest,
    SqlResult,
    SyncStatus,
    Tenant,
    TenantMember,
    TenantRef,
    User,
    WatchEvent,
    Watcher,
)
from ._generated import Namespace as Namespace

# Hand-written list / wrapper types + ergonomics (design).
from .lists import (
    DocumentList,
    MemoryCreateAck,
    MemoryDeleteAck,
    MemoryEditAck,
    MemoryList,
    MemorySearchResponse,
    MemorySearchResultItem,
    NamespaceList,
    SearchResults,
    WatcherList,
)

__all__ = [
    # generated
    "AgentLlmConfig",
    "AgentSession",
    "ApiKey",
    "ChatRequest",
    "ChunkStrategy",
    "Document",
    "DocumentProgress",
    "DocumentTask",
    "GcStatus",
    "LoginResponse",
    "Memory",
    "Namespace",
    "NamespaceCreateRequest",
    "NsAclEntry",
    "NsStats",
    "Quota",
    "QueryFilter",
    "QueryMeta",
    "QueryRequest",
    "QueryResult",
    "QueryResultItem",
    "RegisterRequest",
    "SqlRequest",
    "SqlResult",
    "SyncStatus",
    "Tenant",
    "TenantMember",
    "TenantRef",
    "User",
    "WatchEvent",
    "Watcher",
    # hand-written
    "DocumentList",
    "MemoryCreateAck",
    "MemoryDeleteAck",
    "MemoryEditAck",
    "MemoryList",
    "MemorySearchResponse",
    "MemorySearchResultItem",
    "NamespaceList",
    "SearchResults",
    "WatcherList",
]
