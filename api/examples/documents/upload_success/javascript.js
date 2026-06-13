// POST /api/v1/documents — success (JS raw fetch, async upload)
const resp = await fetch("https://api.cortrix.io/api/v1/documents", {
  method: "POST",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({
    namespace: "contracts",
    filename: "contract_001.pdf",
    content: "...",
    metadata: { source: "legal_dept", tags: ["contract", "2026"] },
  }),
});
const task = await resp.json(); // HTTP 202
console.log("task:", task.task_id, task.status);
