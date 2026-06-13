// POST /api/v1/auth/logout — success
const resp = await fetch("https://api.cortrix.io/api/v1/auth/logout", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
