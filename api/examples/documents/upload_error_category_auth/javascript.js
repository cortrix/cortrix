// POST /api/v1/documents — error category=auth (403, retryable=false)
const resp = await fetch("https://api.cortrix.io/api/v1/documents", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_readonly", "Content-Type": "application/json" },
  body: JSON.stringify({ namespace: "contracts", content: "...", filename: "x.pdf" }),
});
if (resp.status === 403) {
  const err = (await resp.json()).error;
  // retryable=false → update the API Key / request ns:write permission
  console.log("forbidden, required scope:", err.structured_data.required_scope);
}
