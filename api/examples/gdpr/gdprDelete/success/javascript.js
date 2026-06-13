// POST /api/v1/gdpr/delete/{user_id} — success
const resp = await fetch("https://api.cortrix.io/api/v1/gdpr/delete/{user_id}", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
