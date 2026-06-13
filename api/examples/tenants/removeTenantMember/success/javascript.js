// DELETE /api/v1/tenants/{tenant_id}/members/{user_id} — success
const resp = await fetch("https://api.cortrix.io/api/v1/tenants/{tenant_id}/members/{user_id}", {
  method: "DELETE",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
