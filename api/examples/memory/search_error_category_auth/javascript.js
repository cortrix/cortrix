// POST /api/v1/memory/search — error category=auth (401, retryable=false)
const resp = await fetch("https://api.cortrix.io/api/v1/memory/search", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_revoked", "Content-Type": "application/json" },
  body: JSON.stringify({ query: "...", namespace: "user_memory", user_id: "user_001" }),
});
if (resp.status === 401) {
  const err = (await resp.json()).error;
  // retryable=false → re-create the API Key
  console.log(err.code, "— recreate via POST /auth/api-keys");
}
