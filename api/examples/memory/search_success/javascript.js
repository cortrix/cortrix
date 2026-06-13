// POST /api/v1/memory/search — success (JS raw fetch, user_id isolation required)
const resp = await fetch("https://api.cortrix.io/api/v1/memory/search", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({
    query: "the project progress the user mentioned last time", // natural-language memory recall
    namespace: "user_memory",
    user_id: "user_001",
    top_k: 5,
  }),
});
const result = await resp.json();
for (const m of result.memories) console.log(m.memory_type, m.status, m.content);
