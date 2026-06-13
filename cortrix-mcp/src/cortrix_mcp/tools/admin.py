"""F16a admin-scope tools (feature design section 4.6): A1 / A2.

These run under an independent admin scope and are NOT counted in the 29 main tools.
Bearer claim role=admin is required (V1.0 fallback: CORTRIX_MCP_ADMIN=true env var);
otherwise CX_ERR_MCP_ADMIN_REQUIRED (403) is raised.

  A1 cortrix_admin_db_credential_register -> POST /admin/db/credential  (F16a D1) [D3.5]
  A2 cortrix_admin_db_import_run          -> POST /admin/db/import      (F16a D2) [D3.5]

[D3.5] = endpoint not yet present in api/paths/*.yaml (admin.yaml has users/tenants/auth/
config/bootstrap/audit_log only); implemented per the F16a contract and exercised with
mocked HTTP responses during standalone development.
"""

from __future__ import annotations

from typing import Optional

from ..transport import request, require_admin


def register(mcp) -> None:
    @mcp.tool()
    def cortrix_admin_db_credential_register(
        connection_ref: str,
        dsn: str,
        description: str = "",
    ) -> dict:
        """Register a database connection credential (F16a D1; admin only).

        Stores the secret in the encrypted secret store with a 30-day expiry and returns a
        connection_ref handle for later imports (the plaintext secret is never echoed back).

        Args:
            connection_ref: logical handle to register the credential under.
            dsn: database connection string / secret (write-only, stored encrypted).
            description: optional human-readable description.

        Raises CX_ERR_MCP_ADMIN_REQUIRED (403) when the caller lacks the admin role.
        D3.5 deferred: POST /admin/db/credential not yet in api/paths/*.yaml — standalone mock.
        """
        require_admin()
        body: dict = {"connection_ref": connection_ref, "dsn": dsn}
        if description:
            body["description"] = description
        return request("POST", "/admin/db/credential", json_body=body, timeout=15.0)

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

        Raises CX_ERR_MCP_ADMIN_REQUIRED (403) when the caller lacks the admin role.
        D3.5 deferred: POST /admin/db/import not yet in api/paths/*.yaml — standalone mock.
        """
        require_admin()
        body: dict = {"connection_ref": connection_ref, "namespace": namespace}
        if sql is not None:
            body["sql"] = sql
        if table is not None:
            body["table"] = table
        if filter is not None:
            body["filter"] = filter
        return request("POST", "/admin/db/import", json_body=body, timeout=60.0)
