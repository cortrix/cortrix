"""Database import — backs the top-level ``client.import_database()``.

F16a reverse hook. The frozen ``api/paths/import.yaml`` is **not yet built**
(B-R2 briefing) so this is hand-written per design § 2.12 (``POST
/import/database``) and exercised against mocked HTTP responses; the real spec
regen + server integration -> D3.5.

(``import`` is a Python keyword, so the attribute is named ``import_database`` —
matching design § 2.12's top-level ``client.import_database()`` — rather than a
``client.import.*`` namespace.)
"""

from __future__ import annotations

from typing import Any, Optional

from ._base import AsyncResource, SyncResource

PATH_IMPORT_DATABASE = "/import/database"  # import.yaml not yet built -> D3.5


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
    """Manual DB import (F16a). Hand-written; real spec -> D3.5."""

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
