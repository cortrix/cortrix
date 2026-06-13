// GET /api/v1/admin/tenants — success
const resp = await fetch("https://api.cortrix.io/api/v1/admin/tenants", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
