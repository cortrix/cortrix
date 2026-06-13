// PATCH /api/v1/admin/users/{id} — success
const resp = await fetch("https://api.cortrix.io/api/v1/admin/users/{id}", {
  method: "PATCH",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({ /* TODO: payload */ }),
});
const result = await resp.json();
