// PUT /api/v1/system/agent_llm_config — success
const resp = await fetch("https://api.cortrix.io/api/v1/system/agent_llm_config", {
  method: "PUT",
  headers: { "X-API-Key": "cx_live_xxx", "Content-Type": "application/json" },
  body: JSON.stringify({ /* TODO: payload */ }),
});
const result = await resp.json();
