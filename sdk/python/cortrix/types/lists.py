"""Hand-written list / collection wrappers with Pythonic ergonomics.

The spec returns list endpoints as inline envelopes (e.g.
``{"namespaces": [...], "total": N}``); these wrappers give them a typed home
plus ``__iter__`` / ``__len__`` so callers can ``for x in result`` and
``len(result)`` (design § 3.2 / § 4.1).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterator, List, Optional

from ._generated import Document, Memory, Namespace, QueryResultItem, Watcher


@dataclass
class SearchResults:
    """``POST /query`` response. Iterates over ``results`` (QueryResultItem)."""

    results: List[QueryResultItem] = field(default_factory=list)
    meta: Optional[dict[str, Any]] = None

    def __iter__(self) -> Iterator[QueryResultItem]:
        return iter(self.results)

    def __len__(self) -> int:
        return len(self.results)


@dataclass
class DocumentList:
    """``GET /documents`` response (``{documents: [...], total}``)."""

    documents: List[Document] = field(default_factory=list)
    total: Optional[int] = None

    def __iter__(self) -> Iterator[Document]:
        return iter(self.documents)

    def __len__(self) -> int:
        return len(self.documents)


@dataclass
class NamespaceList:
    """``GET /namespaces`` response (``{namespaces: [...], total}``)."""

    namespaces: List[Namespace] = field(default_factory=list)
    total: Optional[int] = None

    def __iter__(self) -> Iterator[Namespace]:
        return iter(self.namespaces)

    def __len__(self) -> int:
        return len(self.namespaces)


@dataclass
class MemoryList:
    """``GET /memory`` response (``{memories: [...]}``)."""

    memories: List[Memory] = field(default_factory=list)
    total: Optional[int] = None
    page: Optional[int] = None
    page_size: Optional[int] = None
    include_invalidated: Optional[bool] = None

    def __iter__(self) -> Iterator[Memory]:
        return iter(self.memories)

    def __len__(self) -> int:
        return len(self.memories)


@dataclass
class MemorySearchResultItem:
    """One ``POST /memory/search`` result item from the live MEM03 wire."""

    interaction_id: Optional[str] = None
    session_id: Optional[str] = None
    query_text: Optional[str] = None
    response_text: Optional[str] = None
    query_type: Optional[str] = None
    score: Optional[float] = None
    expired: Optional[bool] = None
    created_at: Optional[str] = None
    metadata: Optional[str] = None
    block_id: Optional[str] = None
    content: Optional[str] = None
    memory_type: Optional[str] = None
    decay_factor: Optional[float] = None
    final_score: Optional[float] = None


@dataclass
class MemorySearchResponse:
    """``POST /memory/search`` response (``{results,total_results,...}``)."""

    results: List[MemorySearchResultItem] = field(default_factory=list)
    total_results: Optional[int] = None
    latency_ms: Optional[int] = None
    degraded: Optional[bool] = None

    def __iter__(self) -> Iterator[MemorySearchResultItem]:
        return iter(self.results)

    def __len__(self) -> int:
        return len(self.results)


@dataclass
class MemoryCreateAck:
    """``POST /memory`` acknowledgment."""

    memory_id: Optional[str] = None
    status: Optional[str] = None
    namespace: Optional[str] = None
    memory_type: Optional[str] = None
    created_at: Optional[str] = None


@dataclass
class MemoryEditAck:
    """``PATCH /memory/{id}`` acknowledgment."""

    new_memory_id: Optional[str] = None
    invalidated_memory_id: Optional[str] = None
    memory_id: Optional[str] = None
    content: Optional[str] = None
    metadata_json: Optional[str] = None
    updated_at: Optional[str] = None
    extraction_method: Optional[str] = None


@dataclass
class MemoryDeleteAck:
    """``DELETE /memory/{id}`` soft-delete acknowledgment."""

    block_id: Optional[str] = None
    memory_id: Optional[str] = None
    status: Optional[str] = None


@dataclass
class WatcherList:
    """``GET /watch`` response (``{watchers: [...]}``)."""

    watchers: List[Watcher] = field(default_factory=list)
    total: Optional[int] = None

    def __iter__(self) -> Iterator[Watcher]:
        return iter(self.watchers)

    def __len__(self) -> int:
        return len(self.watchers)
