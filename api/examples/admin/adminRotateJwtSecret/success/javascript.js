// POST /api/v1/admin/auth/rotate-jwt-secret — success
const resp = await fetch("https://api.cortrix.io/api/v1/admin/auth/rotate-jwt-secret", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
