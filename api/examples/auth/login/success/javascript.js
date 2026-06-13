// POST /api/v1/auth/login — success
const resp = await fetch("https://api.cortrix.io/api/v1/auth/login", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({ /* TODO: payload */ }),
});
const result = await resp.json();
