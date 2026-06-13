// POST /api/v1/query — error category=auth (403, retryable=false)
const requested = ["contracts", "finance", "hr"];
const resp = await fetch("https://api.cortrix.io/api/v1/query", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({ query: "...", namespaces: requested, top_k: 10 }),
});
if (resp.status === 403) {
  const err = (await resp.json()).error;
  // retryable=false → no retry; degrade to querying only the authorized NS
  const unauthorized = new Set(err.structured_data.unauthorized_namespaces ?? []);
  const allowed = requested.filter((ns) => !unauthorized.has(ns));
  // re-issue the query with allowed...
  console.log("retry with allowed namespaces:", allowed);
}
