// GET /api/v1/namespaces/{ns} — success
const resp = await fetch("https://api.cortrix.io/api/v1/namespaces/{ns}", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
