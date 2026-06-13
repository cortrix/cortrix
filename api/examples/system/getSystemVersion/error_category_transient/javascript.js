// GET /api/v1/system/version — error (HTTP 429)
const resp = await fetch("https://api.cortrix.io/api/v1/system/version", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
if (!resp.ok) {
  const err = (await resp.json()).error;
  // Agent decision: route by err.retryable / err.category
  console.log(err.code, err.category, err.retryable);
}
