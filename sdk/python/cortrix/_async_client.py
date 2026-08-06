"""Asynchronous Cortrix client. Symmetric to :class:`Cortrix`."""

from __future__ import annotations

import asyncio
from typing import Any, Callable, List, Optional, Type, TypeVar, Union, overload

import httpx

from ._async_base import AsyncBaseClient
from ._client import Cortrix
from ._constants import DEFAULT_BASE_URL, DEFAULT_MAX_RETRIES, DEFAULT_TIMEOUT
from .resources.auth import AsyncAuth
from .resources.documents import AsyncDocuments
from .resources.imports import AsyncImports
from .resources.memory import AsyncMemory
from .resources.namespaces import AsyncNamespaces
from .resources.ops import AsyncOpsNamespace
from .resources.query import AsyncQuery
from .resources.sql import AsyncSQL
from .resources.sync import AsyncSync
from .resources.system import AsyncSystem
from .resources.tenants import AsyncTenants
from .resources.watchers import AsyncWatchers

T = TypeVar("T")


class AsyncCortrix(AsyncBaseClient):
    """Cortrix asynchronous client. Interface mirrors :class:`Cortrix`; all
    request methods are ``async``.

    Usage::

        async with AsyncCortrix(base_url="...", api_key="...") as client:
            results = await client.search("contracts", "query")
    """

    def __init__(
        self,
        *,
        base_url: str = DEFAULT_BASE_URL,
        api_key: Optional[str] = None,
        tenant_id: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT,
        max_retries: int = DEFAULT_MAX_RETRIES,
        http_client: Optional[httpx.AsyncClient] = None,
        client_id: Optional[str] = None,
        trace_id_provider: Optional[Callable[[], str]] = None,
    ) -> None:
        super().__init__(
            base_url=base_url,
            api_key=api_key,
            tenant_id=tenant_id,
            timeout=timeout,
            max_retries=max_retries,
            client_id=client_id,
            trace_id_provider=trace_id_provider,
        )
        self._http = (
            http_client if http_client is not None else httpx.AsyncClient(timeout=timeout)
        )
        self._owns_http = http_client is None
        self._documents: Optional[AsyncDocuments] = None
        self._namespaces: Optional[AsyncNamespaces] = None
        self._query: Optional[AsyncQuery] = None
        self._memory: Optional[AsyncMemory] = None
        self._sql: Optional[AsyncSQL] = None
        self._watchers: Optional[AsyncWatchers] = None
        self._sync: Optional[AsyncSync] = None
        self._auth: Optional[AsyncAuth] = None
        self._system: Optional[AsyncSystem] = None
        self._tenants: Optional[AsyncTenants] = None
        self._ops: Optional[AsyncOpsNamespace] = None
        self._imports: Optional[AsyncImports] = None

    @overload
    async def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict[str, Any]] = ...,
        params: Optional[dict[str, Any]] = ...,
        files: Optional[dict[str, Any]] = ...,
        data: Optional[dict[str, Any]] = ...,
        headers: Optional[dict[str, str]] = ...,
        timeout: Optional[float] = ...,
        response_model: Type[T],
    ) -> T: ...

    @overload
    async def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict[str, Any]] = ...,
        params: Optional[dict[str, Any]] = ...,
        files: Optional[dict[str, Any]] = ...,
        data: Optional[dict[str, Any]] = ...,
        headers: Optional[dict[str, str]] = ...,
        timeout: Optional[float] = ...,
        response_model: None = ...,
    ) -> Any: ...

    async def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict[str, Any]] = None,
        params: Optional[dict[str, Any]] = None,
        files: Optional[dict[str, Any]] = None,
        data: Optional[dict[str, Any]] = None,
        headers: Optional[dict[str, str]] = None,
        timeout: Optional[float] = None,
        response_model: Optional[Type[T]] = None,
    ) -> Any:
        url = self._url(path)
        attempt = 0
        while True:
            request_headers = self._build_headers(headers)
            try:
                resp = await self._http.request(
                    method,
                    url,
                    json=json,
                    params=params,
                    files=files,
                    data=data,
                    headers=request_headers,
                    timeout=timeout if timeout is not None else self.timeout,
                )
            except (httpx.ConnectError, httpx.TimeoutException) as e:
                retry, wait = self.should_retry(e, attempt)
                if retry:
                    await asyncio.sleep(wait)
                    attempt += 1
                    continue
                raise Cortrix._network_exception(e) from e

            if resp.is_success:
                return Cortrix._parse_success(resp, response_model)

            retry, wait = self.should_retry(resp, attempt)
            if retry:
                await asyncio.sleep(wait)
                attempt += 1
                continue
            raise self._build_exception(resp)

    # --- Resource properties (lazy) ---

    @property
    def documents(self) -> AsyncDocuments:
        if self._documents is None:
            self._documents = AsyncDocuments(self)
        return self._documents

    @property
    def namespaces(self) -> AsyncNamespaces:
        if self._namespaces is None:
            self._namespaces = AsyncNamespaces(self)
        return self._namespaces

    @property
    def query(self) -> AsyncQuery:
        if self._query is None:
            self._query = AsyncQuery(self)
        return self._query

    @property
    def memory(self) -> AsyncMemory:
        if self._memory is None:
            self._memory = AsyncMemory(self)
        return self._memory

    @property
    def sql(self) -> AsyncSQL:
        if self._sql is None:
            self._sql = AsyncSQL(self)
        return self._sql

    @property
    def watchers(self) -> AsyncWatchers:
        if self._watchers is None:
            self._watchers = AsyncWatchers(self)
        return self._watchers

    @property
    def sync(self) -> AsyncSync:
        if self._sync is None:
            self._sync = AsyncSync(self)
        return self._sync

    @property
    def auth(self) -> AsyncAuth:
        if self._auth is None:
            self._auth = AsyncAuth(self)
        return self._auth

    @property
    def system(self) -> AsyncSystem:
        if self._system is None:
            self._system = AsyncSystem(self)
        return self._system

    @property
    def tenants(self) -> AsyncTenants:
        if self._tenants is None:
            self._tenants = AsyncTenants(self)
        return self._tenants

    @property
    def ops(self) -> AsyncOpsNamespace:
        if self._ops is None:
            self._ops = AsyncOpsNamespace(self)
        return self._ops

    async def search(
        self,
        namespace: Union[str, List[str]],
        query: str,
        *,
        top_k: int = 10,
        rerank: bool = True,
        include_sources: bool = False,
        filters: Optional[dict[str, Any]] = None,
    ) -> Any:
        """Semantic search. ``POST /query``. See :meth:`Cortrix.search`."""
        return await self.query.run(
            namespace,
            query,
            top_k=top_k,
            rerank=rerank,
            include_sources=include_sources,
            filters=filters,
        )

    async def get_sources(self, interaction_id: str) -> Any:
        """Citation sources — endpoint not yet in the published API spec."""
        return await self.query.get_sources(interaction_id)

    async def import_database(
        self,
        namespace: str,
        *,
        connection: dict[str, Any],
        query: Optional[str] = None,
        table: Optional[str] = None,
        mode: str = "per_row",
    ) -> Any:
        """Manual database import. ``POST /import/database`` (import.yaml not yet
        built — formal spec to follow)."""
        if self._imports is None:
            self._imports = AsyncImports(self)
        return await self._imports.database(
            namespace, connection=connection, query=query, table=table, mode=mode
        )

    async def close(self) -> None:
        if self._owns_http:
            await self._http.aclose()

    async def __aenter__(self) -> "AsyncCortrix":
        return self

    async def __aexit__(self, *args: Any) -> None:
        await self.close()
