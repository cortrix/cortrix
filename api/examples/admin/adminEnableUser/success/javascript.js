// POST /api/v1/admin/users/{id}/enable — success
const resp = await fetch("https://api.cortrix.io/api/v1/admin/users/{id}/enable", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
