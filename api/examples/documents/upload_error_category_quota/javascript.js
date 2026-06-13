// POST /api/v1/documents — error category=quota (403, retryable=false)
const resp = await fetch("https://api.cortrix.io/api/v1/documents", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({ namespace: "contracts", content: "...", filename: "x.pdf" }),
});
if (!resp.ok) {
  const err = (await resp.json()).error;
  // category=quota + retryable=false → no retry
  if (err.category === "quota") {
    console.log(`quota full: ${err.structured_data.ns_quota_used}/${err.structured_data.ns_quota_limit}`);
  }
}
