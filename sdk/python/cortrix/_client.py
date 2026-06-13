"""Synchronous Cortrix client. See P03 design § 2.2."""

from __future__ import annotations

import time
from typing import Any, Callable, List, Optional, Type, TypeVar, Union, overload

import httpx

from . import _exceptions as exc
from ._base_client import BaseClient
from ._constants import DEFAULT_BASE_URL, DEFAULT_MAX_RETRIES, DEFAULT_TIMEOUT
from ._models import parse_model
from .resources.auth import Auth
from .resources.documents import Documents
from .resources.imports import Imports
from .resources.memory import Memory_
from .resources.namespaces import Namespaces
from .resources.ops import OpsNamespace
from .resources.query import Query
from .resources.sql import SQL
from .resources.sync import Sync
from .resources.system import System
from .resources.tenants import Tenants
from .resources.watchers import Watchers

T = TypeVar("T")


class Cortrix(BaseClient):
    """Cortrix synchronous client (httpx-based).

    Usage::

        client = Cortrix(base_url="http://localhost:9090", api_key="sk-xxx")
        results = client.search("contracts", "Party A breach clause", top_k=10)
    """

    def __init__(
        self,
        *,
        base_url: str = DEFAULT_BASE_URL,
        api_key: Optional[str] = None,
        tenant_id: Optional[str] = None,
        timeout: float = DEFAULT_TIMEOUT,
        max_retries: int = DEFAULT_MAX_RETRIES,
        http_client: Optional[httpx.Client] = None,
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
        self._http = http_client if http_client is not None else httpx.Client(timeout=timeout)
        self._owns_http = http_client is None
        # lazily-instantiated resource caches
        self._documents: Optional[Documents] = None
        self._namespaces: Optional[Namespaces] = None
        self._query: Optional[Query] = None
        self._memory: Optional[Memory_] = None
        self._sql: Optional[SQL] = None
        self._watchers: Optional[Watchers] = None
        self._sync: Optional[Sync] = None
        self._auth: Optional[Auth] = None
        self._system: Optional[System] = None
        self._tenants: Optional[Tenants] = None
        self._ops: Optional[OpsNamespace] = None
        self._imports: Optional[Imports] = None

    # --- request loop ---

    @overload
    def _request(
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
    def _request(
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

    def _request(
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
                resp = self._http.request(
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
                    time.sleep(wait)
                    attempt += 1
                    continue
                raise self._network_exception(e) from e

            if resp.is_success:
                return self._parse_success(resp, response_model)

            retry, wait = self.should_retry(resp, attempt)
            if retry:
                time.sleep(wait)
                attempt += 1
                continue
            raise self._build_exception(resp)

    @staticmethod
    def _network_exception(e: Exception) -> exc.CortrixError:
        if isinstance(e, httpx.TimeoutException):
            return exc.TimeoutError(f"Request timed out: {e}", category="timeout", retryable=True)
        return exc.ConnectionError(
            f"Connection failed (check base_url / network): {e}",
            category="transient",
            retryable=True,
        )

    @staticmethod
    def _parse_success(resp: httpx.Response, response_model: Optional[Type[T]]) -> Any:
        if resp.status_code == 204 or not resp.content:
            return None
        body = resp.json()
        if response_model is None:
            return body
        return parse_model(response_model, body)

    # --- Resource properties (lazy) ---

    @property
    def documents(self) -> Documents:
        if self._documents is None:
            self._documents = Documents(self)
        return self._documents

    @property
    def namespaces(self) -> Namespaces:
        if self._namespaces is None:
            self._namespaces = Namespaces(self)
        return self._namespaces

    @property
    def query(self) -> Query:
        if self._query is None:
            self._query = Query(self)
        return self._query

    @property
    def memory(self) -> Memory_:
        if self._memory is None:
            self._memory = Memory_(self)
        return self._memory

    @property
    def sql(self) -> SQL:
        if self._sql is None:
            self._sql = SQL(self)
        return self._sql

    @property
    def watchers(self) -> Watchers:
        if self._watchers is None:
            self._watchers = Watchers(self)
        return self._watchers

    @property
    def sync(self) -> Sync:
        if self._sync is None:
            self._sync = Sync(self)
        return self._sync

    @property
    def auth(self) -> Auth:
        if self._auth is None:
            self._auth = Auth(self)
        return self._auth

    @property
    def system(self) -> System:
        if self._system is None:
            self._system = System(self)
        return self._system

    @property
    def tenants(self) -> Tenants:
        if self._tenants is None:
            self._tenants = Tenants(self)
        return self._tenants

    @property
    def ops(self) -> OpsNamespace:
        if self._ops is None:
            self._ops = OpsNamespace(self)
        return self._ops

    # --- top-level shortcuts ---

    def search(
        self,
        namespace: Union[str, List[str]],
        query: str,
        *,
        top_k: int = 10,
        rerank: bool = True,
        include_sources: bool = False,
        filters: Optional[dict[str, Any]] = None,
    ) -> Any:
        """Semantic search. ``POST /query`` (real-arch wire).

        ``namespace`` may be a single string, a list (cross-NS), or ``["*"]``
        (all namespaces). Returns a ``QueryResult`` (``results`` + ``meta``).
        """
        return self.query.run(
            namespace,
            query,
            top_k=top_k,
            rerank=rerank,
            include_sources=include_sources,
            filters=filters,
        )

    def get_sources(self, interaction_id: str) -> Any:
        """Citation sources for an interaction (§2.12-only endpoint
        -> D3.5)."""
        return self.query.get_sources(interaction_id)

    def import_database(
        self,
        namespace: str,
        *,
        connection: dict[str, Any],
        query: Optional[str] = None,
        table: Optional[str] = None,
        mode: str = "per_row",
    ) -> Any:
        """Manual DB import (F16a). ``POST /import/database`` (import.yaml not yet
        built -> D3.5)."""
        if self._imports is None:
            self._imports = Imports(self)
        return self._imports.database(
            namespace, connection=connection, query=query, table=table, mode=mode
        )

    def close(self) -> None:
        if self._owns_http:
            self._http.close()

    def __enter__(self) -> "Cortrix":
        return self

    def __exit__(self, *args: Any) -> None:
        self.close()
