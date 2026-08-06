"""SQL (Text-to-SQL) resource. ``/sql`` (ARCH § 4.1.6, pgcortrix).

This resource follows the implemented HTTP architecture. ``query()`` ->
``POST /sql`` (``SqlResult``). The
per-namespace schema CRUD (``register_schema`` / ``get_schema`` /
``delete_schema``) is **§2.12-only** -> § 2.12 wire ``/namespaces/{ns}/sql/schema``
(API spec to be added). ``query()`` accepts an optional ``namespace``
sent in the body for forward-compat (real-arch ``SqlRequest`` has no namespace).
"""

from __future__ import annotations

from typing import Any, Optional

from ..types import SqlResult
from ._base import AsyncResource, SyncResource

PATH_SQL = "/sql"
# Not yet in the published API spec.
PATH_SQL_SCHEMA = "/namespaces/{ns}/sql/schema"


def _query_body(
    namespace: Optional[str], question: str, max_rows: int, explain: bool
) -> dict[str, Any]:
    body: dict[str, Any] = {"question": question, "max_rows": max_rows, "explain": explain}
    if namespace is not None:
        body["namespace"] = namespace  # real-arch SqlRequest has no namespace; forward-compat
    return body


class SQL(SyncResource):
    """Text-to-SQL (extended deployments). ``POST /sql``."""

    def query(
        self,
        namespace: Optional[str] = None,
        *,
        question: str,
        max_rows: int = 100,
        explain: bool = False,
    ) -> SqlResult:
        """Natural-language DB query. ``POST /sql`` -> SqlResult."""
        return self._client._request(
            "POST",
            PATH_SQL,
            json=_query_body(namespace, question, max_rows, explain),
            response_model=SqlResult,
        )

    def register_schema(self, namespace: str, schema: dict[str, Any]) -> Any:
        """Register DB schema. ``POST /namespaces/{ns}/sql/schema`` (§2.12-only -> D3.5 spec)."""
        return self._client._request(
            "POST", PATH_SQL_SCHEMA.format(ns=namespace), json={"schema": schema}
        )

    def get_schema(self, namespace: str) -> Any:
        """Get registered schema. ``GET /namespaces/{ns}/sql/schema`` (§2.12-only -> D3.5 spec)."""
        return self._client._request("GET", PATH_SQL_SCHEMA.format(ns=namespace))

    def delete_schema(self, namespace: str) -> Any:
        """Delete schema. ``DELETE /namespaces/{ns}/sql/schema`` (§2.12-only -> D3.5 spec)."""
        return self._client._request("DELETE", PATH_SQL_SCHEMA.format(ns=namespace))


class AsyncSQL(AsyncResource):
    """Async SQL (symmetric to :class:`SQL`)."""

    async def query(
        self,
        namespace: Optional[str] = None,
        *,
        question: str,
        max_rows: int = 100,
        explain: bool = False,
    ) -> SqlResult:
        return await self._client._request(
            "POST",
            PATH_SQL,
            json=_query_body(namespace, question, max_rows, explain),
            response_model=SqlResult,
        )

    async def register_schema(self, namespace: str, schema: dict[str, Any]) -> Any:
        return await self._client._request(
            "POST", PATH_SQL_SCHEMA.format(ns=namespace), json={"schema": schema}
        )

    async def get_schema(self, namespace: str) -> Any:
        return await self._client._request("GET", PATH_SQL_SCHEMA.format(ns=namespace))

    async def delete_schema(self, namespace: str) -> Any:
        return await self._client._request("DELETE", PATH_SQL_SCHEMA.format(ns=namespace))
