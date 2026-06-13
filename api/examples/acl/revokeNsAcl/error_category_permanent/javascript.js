// DELETE /api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id} — error (HTTP 404)
const resp = await fetch("https://api.cortrix.io/api/v1/namespaces/{ns_id}/acl/{grantee_tenant_id}", {
  method: "DELETE",
  headers: { "X-API-Key": "cx_live_xxx" },
});
if (!resp.ok) {
  const err = (await resp.json()).error;
  // Agent decision: route by err.retryable / err.category
  console.log(err.code, err.category, err.retryable);
}
