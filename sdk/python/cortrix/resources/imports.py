"""Database import — backs the top-level ``client.import_database()``.

DB import reverse hook. The frozen ``api/paths/import.yaml`` is **not yet built**,
so this is hand-written per design (``POST
/import/database``) and exercised against mocked HTTP responses; the real spec
regen + server wiring land at integration.

(``import`` is a Python keyword, so the attribute is named ``import_database`` —
matching design's top-level ``client.import_database()`` — rather than a
``client.import.*`` namespace.)
"""

from __future__ import annotations

from typing import Any, Optional

from ._base import AsyncResource, SyncResource

PATH_IMPORT_DATABASE = "/import/database"  # import.yaml not yet built; spec to follow


def _import_body(
    namespace: str,
    connection: dict[str, Any],
    query: Optional[str],
    table: Optional[str],
    mode: str,
) -> dict[str, Any]:
    body: dict[str, Any] = {"namespace": namespace, "connection": connection, "mode": mode}
    if query is not None:
        body["query"] = query
    if table is not None:
        body["table"] = table
    return body


class Imports(SyncResource):
    """Manual database import. Hand-written; formal spec to follow."""

    def database(
        self,
        namespace: str,
        *,
        connection: dict[str, Any],
        query: Optional[str] = None,
        table: Optional[str] = None,
        mode: str = "per_row",
    ) -> Any:
        """Import DB rows into a namespace. ``POST /import/database``."""
        return self._client._request(
            "POST",
            PATH_IMPORT_DATABASE,
            json=_import_body(namespace, connection, query, table, mode),
        )


class AsyncImports(AsyncResource):
    """Async Imports (symmetric to :class:`Imports`)."""

    async def database(
        self,
        namespace: str,
        *,
        connection: dict[str, Any],
        query: Optional[str] = None,
        table: Optional[str] = None,
        mode: str = "per_row",
    ) -> Any:
        return await self._client._request(
            "POST",
            PATH_IMPORT_DATABASE,
            json=_import_body(namespace, connection, query, table, mode),
        )
