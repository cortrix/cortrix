#!/usr/bin/env bash
# DELETE /api/v1/tenants/{tenant_id}/members/{user_id} — error (HTTP 404)
curl -X DELETE "https://api.cortrix.io/api/v1/tenants/{tenant_id}/members/{user_id}" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 404, see response.json
