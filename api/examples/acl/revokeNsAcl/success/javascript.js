// DELETE /api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id} — success
const resp = await fetch("https://api.cortrix.io/api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id}", {
  method: "DELETE",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
