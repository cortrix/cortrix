// GET /api/v1/tenants/{tenant_id}/members — success
const resp = await fetch("https://api.cortrix.io/api/v1/tenants/{tenant_id}/members", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
