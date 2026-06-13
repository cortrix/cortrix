// GET /api/v1/auth/me — success
const resp = await fetch("https://api.cortrix.io/api/v1/auth/me", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
