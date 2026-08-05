"""Auto-generated request/response models — DO NOT EDIT BY HAND.

Generated from the frozen OpenAPI spec by ``scripts/generate_types.py``
(hand-written generator standing in for openapi-generator-cli, design § S4).
Regenerate with::

    python scripts/generate_types.py

The hand-written ``cortrix/types/*.py`` modules import from here and add Pythonic
ergonomics (``__iter__`` / ``__len__`` / docstrings).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, List, Literal, Optional

@dataclass
class ChunkStrategy:
    """ChunkStrategy (generated)."""
    max_tokens: Optional[int] = None
    overlap_tokens: Optional[int] = None


@dataclass
class NamespaceCreateRequest:
    """NamespaceCreateRequest (generated)."""
    name: str
    display_name: Optional[str] = None
    description: Optional[str] = None
    embedding_model: Optional[str] = None
    chunk_strategy: Optional["ChunkStrategy"] = None
    visibility: Optional[Literal["private", "tenant", "public"]] = None
    isolation_mode: Optional[Literal["shared", "dedicated"]] = None


@dataclass
class Namespace:
    """Namespace (generated)."""
    namespace: str
    status: Literal["active", "creating", "deleting", "error"]
    display_name: Optional[str] = None
    description: Optional[str] = None
    embedding_model: Optional[str] = None
    chunk_strategy: Optional["ChunkStrategy"] = None
    visibility: Optional[Literal["private", "tenant", "public"]] = None
    isolation_mode: Optional[Literal["shared", "dedicated"]] = None
    owner_node_id: Optional[str] = None
    ns_acl: Optional[List["NsAclEntry"]] = None
    created_at: Optional[str] = None


@dataclass
class DocumentProgress:
    """DocumentProgress (generated)."""
    stage: Optional[Literal["pending", "parsing", "chunking", "embedding", "indexing", "ready", "failed"]] = None
    chunks_total: Optional[int] = None
    chunks_done: Optional[int] = None
    semantic_score: Optional[float] = None


@dataclass
class Document:
    """Document (generated)."""
    document_id: str
    status: Literal["pending", "parsing", "chunking", "embedding", "indexing", "ready", "failed"]
    namespace: Optional[str] = None
    filename: Optional[str] = None
    source_type: Optional[str] = None
    source_ref: Optional[str] = None
    progress: Optional["DocumentProgress"] = None
    content_hash: Optional[str] = None
    metadata: Optional[Dict[str, Any]] = None
    created_at: Optional[str] = None


@dataclass
class DocumentTask:
    """async upload task."""
    task_id: str
    status: Literal["submitted", "queued", "processing", "ready", "failed", "cancelled"]
    document_id: Optional[str] = None
    namespace: Optional[str] = None
    progress: Optional["DocumentProgress"] = None
    created_at: Optional[str] = None


@dataclass
class QueryFilter:
    """QueryFilter (generated)."""
    tags: Optional[List[str]] = None
    date_range: Optional[Dict[str, Any]] = None


@dataclass
class QueryRequest:
    """QueryRequest (generated)."""
    query: str
    namespaces: List[str]
    top_k: Optional[int] = None
    rerank: Optional[bool] = None
    filter: Optional["QueryFilter"] = None
    include_sources: Optional[bool] = None


@dataclass
class QueryResultItem:
    """QueryResultItem (generated)."""
    child_id: Optional[str] = None
    parent_id: Optional[str] = None
    content: Optional[str] = None
    parent_content: Optional[str] = None
    score: Optional[float] = None
    rerank_score: Optional[float] = None
    namespace: Optional[str] = None
    content_hash: Optional[str] = None
    metadata: Optional[Dict[str, Any]] = None


@dataclass
class QueryMeta:
    """Class-A fields (data integrity) — always present; cross-NS partial-success semantics."""
    namespaces_queried: List[str]
    namespaces_succeeded: List[str]
    coverage_ratio: float
    latency_ms: int
    namespaces_failed: Optional[List[Dict[str, Any]]] = None
    deduplicated_chunks: Optional[List[str]] = None
    deduplicated_chunks_count: Optional[int] = None
    warnings: Optional[List[Dict[str, Any]]] = None


@dataclass
class QueryResult:
    """QueryResult (generated)."""
    results: List["QueryResultItem"]
    meta: "QueryMeta"


@dataclass
class MemorySearchRequest:
    """MemorySearchRequest (generated)."""
    query: str
    namespace: str
    user_id: str
    top_k: Optional[int] = None


@dataclass
class MemoryLogRequest:
    """MemoryLogRequest (generated)."""
    namespace: str
    user_id: str
    query: str
    response: str
    session_id: Optional[str] = None
    context: Optional[Dict[str, Any]] = None


@dataclass
class MemoryCreateRequest:
    """MemoryCreateRequest (generated)."""
    namespace: str
    content: str
    memory_type: Literal["fact", "preference", "event"]
    metadata: Optional[Dict[str, Any]] = None


@dataclass
class Memory:
    """Memory (generated)."""
    memory_id: str
    content: str
    memory_type: Literal["fact", "preference", "event"]
    status: Literal["valid", "invalidated"]
    source_session_id: Optional[str] = None
    source_interaction_id: Optional[str] = None
    created_at: Optional[str] = None
    score: Optional[float] = None


@dataclass
class SqlRequest:
    """SqlRequest (generated)."""
    question: str
    max_rows: Optional[int] = None
    explain: Optional[bool] = None


@dataclass
class SqlResult:
    """SqlResult (generated)."""
    sql: str
    results: List[Dict[str, Any]]
    row_count: int
    explanation: Optional[str] = None
    execution_time_ms: Optional[int] = None


@dataclass
class SyncStatus:
    """SyncStatus (generated)."""
    status: Literal["idle", "running", "stopped", "error"]
    last_sync_at: Optional[str] = None
    synced_documents: Optional[int] = None
    pending_documents: Optional[int] = None


@dataclass
class Watcher:
    """Watcher (generated)."""
    id: str
    path: str
    target_namespaces: Optional[List[str]] = None
    recursive: Optional[bool] = None
    status: Optional[Literal["active", "paused", "error"]] = None


@dataclass
class WatchEvent:
    """WatchEvent (generated)."""
    event_id: Optional[str] = None
    watcher_id: Optional[str] = None
    event_type: Optional[Literal["created", "modified", "deleted", "moved"]] = None
    path: Optional[str] = None
    namespace: Optional[str] = None
    timestamp: Optional[str] = None


@dataclass
class RegisterRequest:
    """RegisterRequest (generated)."""
    email: str
    password: str
    display_name: Optional[str] = None


@dataclass
class TenantRef:
    """TenantRef (generated)."""
    id: Optional[str] = None
    name: Optional[str] = None
    role: Optional[Literal["owner", "admin", "member"]] = None


@dataclass
class User:
    """User (generated)."""
    id: str
    email: str
    display_name: Optional[str] = None
    status: Optional[Literal["active", "disabled", "pending_verification"]] = None
    tenants: Optional[List["TenantRef"]] = None
    created_at: Optional[str] = None


@dataclass
class LoginResponse:
    """LoginResponse (generated)."""
    access_token: str
    refresh_token: str
    expires_in: int
    user: Optional["User"] = None


@dataclass
class ApiKey:
    """ApiKey (generated)."""
    id: str
    prefix: str
    name: Optional[str] = None
    key: Optional[str] = None
    created_at: Optional[str] = None
    last_used_at: Optional[str] = None


@dataclass
class Quota:
    """Quota (generated)."""
    max_namespaces: Optional[int] = None
    max_documents: Optional[int] = None
    max_storage_bytes: Optional[int] = None
    used_namespaces: Optional[int] = None
    used_documents: Optional[int] = None
    used_storage_bytes: Optional[int] = None


@dataclass
class TenantMember:
    """TenantMember (generated)."""
    user_id: str
    role: Literal["owner", "admin", "member"]
    email: Optional[str] = None
    joined_at: Optional[str] = None


@dataclass
class Tenant:
    """Tenant (generated)."""
    tenant_id: str
    name: str
    owner_user_id: Optional[str] = None
    quota: Optional["Quota"] = None
    created_at: Optional[str] = None


@dataclass
class NsAclEntry:
    """NsAclEntry (generated)."""
    grantee_tenant_id: str
    permission: Literal["read", "write"]
    granted_at: Optional[str] = None


@dataclass
class ChatRequest:
    """ChatRequest (generated)."""
    message: str
    session_id: Optional[str] = None
    namespace: Optional[str] = None


@dataclass
class AgentSession:
    """AgentSession (generated)."""
    session_id: str
    tenant_id: Optional[str] = None
    messages: Optional[List[Dict[str, Any]]] = None
    created_at: Optional[str] = None


@dataclass
class AgentLlmConfig:
    """agent LLM config (GET redacted read / PUT admin-only update)."""
    provider: Optional[Literal["openai", "deepseek", "azure"]] = None
    model: Optional[str] = None
    api_key_set: Optional[bool] = None
    base_url: Optional[str] = None


@dataclass
class NsStats:
    """Per-NS metrics dict (V3 resolution 10, replaces the PromQL ns_id label)."""
    namespace: Optional[str] = None
    document_count: Optional[int] = None
    chunk_count: Optional[int] = None
    storage_bytes: Optional[int] = None
    last_query_at: Optional[str] = None


@dataclass
class GcStatus:
    """GcStatus (generated)."""
    status: Optional[Literal["idle", "running"]] = None
    soft_deleted_count: Optional[int] = None
    reclaimable_bytes: Optional[int] = None
    last_gc_at: Optional[str] = None


__all__ = ["ChunkStrategy", "NamespaceCreateRequest", "Namespace", "DocumentProgress", "Document", "DocumentTask", "QueryFilter", "QueryRequest", "QueryResultItem", "QueryMeta", "QueryResult", "MemorySearchRequest", "MemoryLogRequest", "MemoryCreateRequest", "Memory", "SqlRequest", "SqlResult", "SyncStatus", "Watcher", "WatchEvent", "RegisterRequest", "TenantRef", "User", "LoginResponse", "ApiKey", "Quota", "TenantMember", "Tenant", "NsAclEntry", "ChatRequest", "AgentSession", "AgentLlmConfig", "NsStats", "GcStatus"]
