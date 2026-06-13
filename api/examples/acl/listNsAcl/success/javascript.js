// GET /api/v1/namespaces/{ns_id}/acl — success
const resp = await fetch("https://api.cortrix.io/api/v1/namespaces/{ns_id}/acl", {
  method: "GET",
  headers: { "X-API-Key": "cx_live_xxx" },
});
const result = await resp.json();
