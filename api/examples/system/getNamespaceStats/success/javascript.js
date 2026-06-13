// GET /api/v1/system/namespaces/{ns}/stats — success
const resp = await fetch("https://api.cortrix.io/api/v1/system/namespaces/{ns}/stats", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
