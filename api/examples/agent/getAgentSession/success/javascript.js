// GET /api/v1/agent/sessions/{session_id} — success
const resp = await fetch("https://api.cortrix.io/api/v1/agent/sessions/{session_id}", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
