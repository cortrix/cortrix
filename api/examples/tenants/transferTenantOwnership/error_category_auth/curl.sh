#!/usr/bin/env bash
# POST /api/v1/tenants/{tenant_id}/transfer-ownership — error (HTTP 401)
curl -X POST "https://api.cortrix.io/api/v1/tenants/{tenant_id}/transfer-ownership" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload; see components/schemas */ }'
# → HTTP 401, see response.json
