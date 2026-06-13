// POST /api/v1/sync/start — success
const resp = await fetch("https://api.cortrix.io/api/v1/sync/start", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({ /* TODO: payload */ }),
});
const result = await resp.json();
