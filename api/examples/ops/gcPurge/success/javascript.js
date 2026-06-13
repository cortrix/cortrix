// POST /api/v1/gc/purge — success
const resp = await fetch("https://api.cortrix.io/api/v1/gc/purge", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
