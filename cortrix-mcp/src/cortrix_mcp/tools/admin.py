"""F16a admin-scope tools (feature design section 4.6): A1 / A2.

These run under an independent admin scope and are NOT counted in the 29 main tools.
Bearer claim role=admin is required (V1.0 fallback: CORTRIX_MCP_ADMIN=true env var);
otherwise CX_ERR_MCP_ADMIN_REQUIRED (403) is raised.

  A1 cortrix_admin_db_credential_register -> POST /admin/db-connections  (F16a D1)
  A2 cortrix_admin_db_import_run          -> POST /import/database        (F16a D2)

Both routes are live in the backend (import_routes.cpp) under kPermAdmin; the register
route additionally sits behind the /api/v1/admin/* AdminGuard prefix.
"""

from __future__ import annotations

from typing import Optional

from ..transport import request, require_admin

_CONNECTION_REF_ALIASES: dict[str, str] = {}


def register(mcp) -> None:
    @mcp.tool()
    def cortrix_admin_db_credential_register(
        name: str = "",
        dsn: str = "",
        expire_days: Optional[int] = None,
        connection_ref: Optional[str] = None,
        description: Optional[str] = None,
    ) -> dict:
        """Register a database connection credential (F16a D1; admin only).

        Stores the secret in the encrypted secret store and returns a ref_id handle for
        later imports (the plaintext secret is never echoed back).

        Args:
            name: logical name to register the credential under (returned as ref_id's label).
            dsn: database connection string / secret (write-only, stored encrypted).
            expire_days: optional credential expiry in days (backend default applies if omitted).
            connection_ref: compatibility alias for older agent call sites; maps to name.
            description: compatibility metadata accepted by older agent call sites.

        Maps to POST /admin/db-connections (import_routes.cpp); the backend requires name + dsn.
        Raises CX_ERR_MCP_ADMIN_REQUIRED (403) when the caller lacks the admin role.
        """
        require_admin()
        alias = connection_ref or name
        body: dict = {"name": name or connection_ref or "", "dsn": dsn}
        if expire_days is not None:
            body["expire_days"] = expire_days
        result = request("POST", "/admin/db-connections", json_body=body, timeout=15.0)
        data = result.get("data") if isinstance(result, dict) else {}
        if alias and isinstance(data, dict):
            ref_id = data.get("ref_id") or data.get("connection_ref")
            if isinstance(ref_id, str) and ref_id:
                _CONNECTION_REF_ALIASES[alias] = ref_id
        return result

    @mcp.tool()
    def cortrix_admin_db_import_run(
        connection_ref: str,
        namespace: str,
        table: Optional[str] = None,
        filter: Optional[str] = None,
        sql: Optional[str] = None,
    ) -> dict:
        """Trigger a database import run (F16a D2; admin only).

        Two modes (mutually exclusive):
          * table + filter — import rows from ``table`` matching an optional ``filter``.
          * sql            — import the result set of a raw ``sql`` query.

        Args:
            connection_ref: a credential previously registered via the register tool.
            namespace: target namespace to import into.
            table: source table name (table mode).
            filter: optional row filter (table mode).
            sql: raw SQL query (SQL mode).

        Exactly one of {table, sql} must be provided (the backend rejects neither/both with
        CX_ERR_F16A_INVALID_SQL). Maps to POST /import/database (import_routes.cpp).
        Raises CX_ERR_MCP_ADMIN_REQUIRED (403) when the caller lacks the admin role.
        """
        require_admin()
        resolved_ref = _CONNECTION_REF_ALIASES.get(connection_ref, connection_ref)
        body: dict = {"connection_ref": resolved_ref, "namespace": namespace}
        if sql is not None:
            body["sql"] = sql
        if table is not None:
            body["table"] = table
        if filter is not None:
            body["filter"] = filter
        return request("POST", "/import/database", json_body=body, timeout=60.0)
