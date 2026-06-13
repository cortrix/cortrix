// GET /api/v1/admin/audit_log — success
const resp = await fetch("https://api.cortrix.io/api/v1/admin/audit_log", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
