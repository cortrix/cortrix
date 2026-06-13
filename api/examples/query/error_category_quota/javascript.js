// POST /api/v1/query — error category=quota (429, retryable=true)
async function queryWithRetry(body) {
  let resp = await fetch("https://api.cortrix.io/api/v1/query", {
    method: "POST",
    headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (resp.status === 429) {
    const err = (await resp.json()).error;
    // retryable=true → back off per retry_after_ms and retry
    await new Promise((r) => setTimeout(r, err.retry_after_ms ?? 1000));
    resp = await fetch("https://api.cortrix.io/api/v1/query", {
      method: "POST",
      headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
  }
  return resp.json();
}
await queryWithRetry({ query: "...", namespaces: ["contracts"], top_k: 10 });
