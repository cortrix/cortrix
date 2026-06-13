// POST /api/v1/gc/run — success
const resp = await fetch("https://api.cortrix.io/api/v1/gc/run", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
