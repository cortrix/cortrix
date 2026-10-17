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
    source_type: Optional[Literal["upload", "http_upload", "watch_dir", "memory_session", "memory"]] = None
    source_ref: Optional[str] = None
    progress: Optional["DocumentProgress"] = None
    content_hash: Optional[str] = None
    metadata: Optional[Dict[str, Any]] = None
    created_at: Optional[str] = None


@dataclass
class DocumentTask:
    """Asynchronous upload task."""
    task_id: str
    status: Literal["queued", "processing", "cancelling", "completed", "failed", "cancelled"]
    document_id: Optional[str] = None
    namespace: Optional[str] = None
    progress: Optional["DocumentProgress"] = None
    created_at: Optional[str] = None


@dataclass
class UploadDocumentResult:
    """Synchronous result of the multipart POST /namespaces/{ns}/documents file upload. Unlike the async POST /documents (which returns a task_id), this returns the doc_id directly. status reflects the outcome: pending (a new document, processing queued), updating (a new version of a filename already stored, processing queued) or skipped (unchanged content, not processed again)."""
    doc_id: str
    content_hash: str
    status: str
    source_path: str
    message: str
    superseded_doc_id: Optional[str] = None


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
    route: Optional[Literal["auto", "simple", "complex", "chat"]] = None
    rag_fusion: Optional[bool] = None
    include_parent: Optional[bool] = None
    parent_expand_top_k: Optional[int] = None
    parent_max_bytes: Optional[int] = None
    search_config: Optional["SearchConfig"] = None
    timeout_ms: Optional[int] = None


@dataclass
class SearchConfig:
    """Per-query retrieval route toggles. Each route runs unless its flag is false; for example {"enable_bm25": false} skips the lexical route. A flag that is not a boolean is ignored."""
    enable_vector: Optional[bool] = None
    enable_bm25: Optional[bool] = None
    enable_sparse: Optional[bool] = None


@dataclass
class QueryResultItem:
    """QueryResultItem (generated)."""
    child_id: Optional[str] = None
    parent_id: Optional[str] = None
    content: Optional[str] = None
    parent_content: Optional[str] = None
    evidence_kind: Optional[Literal["chunk", "document_summary", "document_label"]] = None
    score: Optional[float] = None
    rerank_score: Optional[float] = None
    namespace: Optional[str] = None
    content_hash: Optional[str] = None
    metadata: Optional[Dict[str, Any]] = None
    score_signals: Optional[Dict[str, Any]] = None


@dataclass
class QueryMeta:
    """Metadata present on every query result: which namespaces were queried and answered, coverage and latency, and per-namespace failures."""
    namespaces_queried: List[str]
    namespaces_succeeded: List[str]
    coverage_ratio: float
    latency_ms: int
    namespaces_failed: Optional[List[Dict[str, Any]]] = None
    deduplicated_chunks: Optional[List[Dict[str, Any]]] = None
    deduplicated_chunks_count: Optional[int] = None
    warnings: Optional[List[Dict[str, Any]]] = None
    via_path: Optional[str] = None
    crag_verdict: Optional[str] = None
    crag_action: Optional[str] = None


@dataclass
class QueryResult:
    """QueryResult (generated)."""
    results: List["QueryResultItem"]
    meta: "QueryMeta"
    explain: Optional[Dict[str, Any]] = None
    error: Optional[Dict[str, Any]] = None


@dataclass
class MemorySearchRequest:
    """MemorySearchRequest (generated)."""
    query: str
    namespace: str
    user_id: str
    top_k: Optional[int] = None


@dataclass
class MemorySearchResultItem:
    """One conversation-memory search hit."""
    interaction_id: str
    session_id: str
    query_text: str
    response_text: str
    score: float
    expired: bool
    created_at: str
    query_type: Optional[str] = None
    metadata: Optional[str] = None


@dataclass
class MemorySearchResponse:
    """POST /memory/search body. Semantic search over conversation memory, isolated per user."""
    results: List["MemorySearchResultItem"]
    total_results: int
    latency_ms: int
    degraded: bool


@dataclass
class MemoryExtractedItem:
    """MemoryExtractedItem (generated)."""
    block_id: str
    type: Literal["fact", "preference", "event"]
    content: str
    confidence: float


@dataclass
class MemoryExtractResponse:
    """POST /memory/extract body. error is always null on success (stable schema)."""
    extracted_memories: List["MemoryExtractedItem"]
    extract_meta: Optional[Dict[str, Any]] = None
    error: Optional[Dict[str, Any]] = None


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
class ImportRequest:
    """Database import request (POST /import/database). Exactly one of {table, sql} must be"""
    namespace: str
    connection_ref: str
    text_strategy: Optional[Literal["per_row", "merge"]] = None
    merge_size: Optional[int] = None
    table: Optional[str] = None
    filter: Optional[Dict[str, Any]] = None
    columns: Optional[List[str]] = None
    sql: Optional[str] = None


@dataclass
class ImportTaskStarted:
    """Acceptance ack for POST /import/database. status is always 'queued'; live status comes from GET /import/tasks/{task_id}/progress."""
    task_id: str
    status: Literal["queued"]
    namespace: str
    connection_ref: str
    estimated_rows: int
    queued_at: str
    progress_endpoint: str
    cancel_endpoint: str
    error: Optional[Dict[str, Any]] = None


@dataclass
class ImportTaskProgress:
    """Import task progress snapshot. error is set only in the FAILED terminal state; cancel resolves to status=cancelled with no error."""
    task_id: str
    status: Literal["queued", "running", "completed", "failed", "cancelling", "cancelled"]
    progress: float
    rows_imported: int
    rows_total: int
    rows_failed: int
    started_at: Optional[str] = None
    estimated_completion_at: Optional[str] = None
    error: Optional[Dict[str, Any]] = None


@dataclass
class DbConnection:
    """A registered import connection as listed by GET /admin/db-connections. The DSN is never returned."""
    ref_id: str
    name: str
    host: str
    db_name: str
    registered_by: str
    revoked: bool


@dataclass
class TraceEntry:
    """One agent trace row. Optional columns are JSON null when unset (stable schema)."""
    method: str
    duration_ms: int
    source: str
    status: str
    created_at: int
    session_id: Optional[str] = None
    trace_id: Optional[str] = None
    agent_id: Optional[str] = None
    params: Optional[str] = None
    result_summary: Optional[str] = None
    error_code: Optional[str] = None
    namespace_id: Optional[str] = None


@dataclass
class TraceSessionMeta:
    """TraceSessionMeta (generated)."""
    total_duration_ms: int
    total_count: int
    has_next: bool
    response_size_warning: bool
    session_start: Optional[int] = None
    session_end: Optional[int] = None
    next_offset: Optional[int] = None


@dataclass
class TraceSession:
    """GET /traces/{session_id} body."""
    session_id: str
    trace_count: int
    traces: List["TraceEntry"]
    meta: "TraceSessionMeta"


@dataclass
class InteractionListItem:
    """InteractionListItem (generated)."""
    interaction_id: str
    session_id: str
    user_id: str
    namespace_id: str
    role: str
    query_text: str
    query_type: str
    created_at: int


@dataclass
class InteractionListMeta:
    """InteractionListMeta (generated)."""
    total_count: int
    has_next: bool
    next_offset: Optional[int] = None


@dataclass
class InteractionList:
    """GET /interactions body."""
    interactions: List["InteractionListItem"]
    meta: "InteractionListMeta"


@dataclass
class InteractionSource:
    """InteractionSource (generated)."""
    source_id: str
    source_type: str
    snippet: str
    relevance_score: float


@dataclass
class InteractionSources:
    """GET /interactions/{id}/sources body."""
    interaction_id: str
    source_count: int
    deleted_sources_count: int
    sources: List["InteractionSource"]


@dataclass
class OperationLogEntry:
    """One operation log row. Optional columns are JSON null when unset."""
    id: str
    timestamp: int
    user_id: str
    action: str
    resource_type: str
    namespace_id: Optional[str] = None
    resource_id: Optional[str] = None
    summary: Optional[str] = None
    trace_id: Optional[str] = None
    session_id: Optional[str] = None


@dataclass
class OperationLogPage:
    """GET /operations body. Pagination is offset-based (no opaque cursor)."""
    operations: List["OperationLogEntry"]
    total_count: int
    limit: int
    offset: int
    meta: Dict[str, Any]


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
    """Agent LLM configuration (GET returns it redacted; PUT is an admin-only update)."""
    provider: Optional[Literal["openai", "deepseek", "azure"]] = None
    model: Optional[str] = None
    api_key_set: Optional[bool] = None
    base_url: Optional[str] = None


@dataclass
class NsStats:
    """Per-namespace metrics dictionary."""
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
    last_gc_at: Optional[int] = None


@dataclass
class BatchSubmitDocument:
    """One document in a batch submission."""
    doc_id: str
    content: str
    metadata: Optional[Dict[str, Any]] = None


@dataclass
class BatchSubmitOptions:
    """Batch submission options."""
    async_: Optional[bool] = None
    on_duplicate: Optional[Literal["skip", "overwrite", "error"]] = None


@dataclass
class BatchSubmitRequest:
    """BatchSubmitRequest (generated)."""
    namespace: str
    documents: List["BatchSubmitDocument"]
    options: Optional["BatchSubmitOptions"] = None


@dataclass
class BatchSubmitResultItem:
    """Accepted per-document submission result (an entry of results[])."""
    doc_id: str
    task_id: str
    status: Literal["submitted", "skipped"]


@dataclass
class BatchSubmitFailureItem:
    """Per-document failure in the standard error fields. error_code is the existing CX_ERR_* code of the step that failed."""
    doc_id: str
    error_code: str
    category: Literal["auth", "quota", "transient", "permanent", "timeout"]
    retryable: bool
    retry_after_ms: Optional[int] = None
    structured_data: Optional[Dict[str, Any]] = None


@dataclass
class BatchSubmitMeta:
    """Partial-success meta; coverage_ratio = succeeded / total_submitted."""
    succeeded: List[str]
    failed: List["BatchSubmitFailureItem"]
    coverage_ratio: float
    total_submitted: int


@dataclass
class BatchSubmitResponse:
    """Batch submission partial-success response. HTTP 200 even with per-document failures; a rejection of the whole batch returns the standard error response."""
    results: List["BatchSubmitResultItem"]
    meta: "BatchSubmitMeta"


__all__ = ["ChunkStrategy", "NamespaceCreateRequest", "Namespace", "DocumentProgress", "Document", "DocumentTask", "UploadDocumentResult", "QueryFilter", "QueryRequest", "SearchConfig", "QueryResultItem", "QueryMeta", "QueryResult", "MemorySearchRequest", "MemorySearchResultItem", "MemorySearchResponse", "MemoryExtractedItem", "MemoryExtractResponse", "MemoryCreateRequest", "Memory", "ImportRequest", "ImportTaskStarted", "ImportTaskProgress", "DbConnection", "TraceEntry", "TraceSessionMeta", "TraceSession", "InteractionListItem", "InteractionListMeta", "InteractionList", "InteractionSource", "InteractionSources", "OperationLogEntry", "OperationLogPage", "Watcher", "WatchEvent", "RegisterRequest", "TenantRef", "User", "LoginResponse", "ApiKey", "Quota", "TenantMember", "Tenant", "NsAclEntry", "ChatRequest", "AgentSession", "AgentLlmConfig", "NsStats", "GcStatus", "BatchSubmitDocument", "BatchSubmitOptions", "BatchSubmitRequest", "BatchSubmitResultItem", "BatchSubmitFailureItem", "BatchSubmitMeta", "BatchSubmitResponse"]
