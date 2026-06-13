// POST /api/v1/memory/search — error category=timeout (504, retryable=true)
const body = { query: "...", namespace: "user_memory", user_id: "user_001" };
for (let attempt = 0; attempt < 3; attempt++) {
  const resp = await fetch("https://api.cortrix.io/api/v1/memory/search", {
    method: "POST",
    headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (resp.ok) break;
  if (resp.status === 504) {
    const err = (await resp.json()).error;
    // retryable=true → back off and retry
    await new Promise((r) => setTimeout(r, err.retry_after_ms ?? 1000));
  }
}
