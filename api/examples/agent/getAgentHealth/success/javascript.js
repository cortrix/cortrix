// GET /api/v1/agent/health — success
const resp = await fetch("https://api.cortrix.io/api/v1/agent/health", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
