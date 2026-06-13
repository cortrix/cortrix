// POST /api/v1/query — success (JS raw fetch; usable directly by JS agents such as LangChain.js / LlamaIndex.ts)
const response = await fetch("https://api.cortrix.io/api/v1/query", {
  method: "POST",
  headers: {
    "X-API-Key": "cx_live_xxx",
    "Content-Type": "application/json",
  },
  body: JSON.stringify({
    query: "Party A breach-of-contract clause", // natural-language semantic query
    namespaces: ["contracts", "support_docs"],
    top_k: 10,
    rerank: true,
  }),
});
const result = await response.json();

// Class-A meta: partial-success semantics
for (const item of result.results) {
  console.log(item.score, item.namespace, item.content.slice(0, 50));
}
console.log("coverage:", result.meta.coverage_ratio, "failed:", result.meta.namespaces_failed);
