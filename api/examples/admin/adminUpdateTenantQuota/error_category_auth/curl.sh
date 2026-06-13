#!/usr/bin/env bash
# PATCH /api/v1/admin/tenants/{id}/quota — error (HTTP 401)
curl -X PATCH "https://api.cortrix.io/api/v1/admin/tenants/{id}/quota" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
# → HTTP 401, see response.json
