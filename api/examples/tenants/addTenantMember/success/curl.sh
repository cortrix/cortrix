#!/usr/bin/env bash
# POST /api/v1/tenants/{tenant_id}/members — success
curl -X POST "https://api.cortrix.io/api/v1/tenants/{tenant_id}/members" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload; see components/schemas */ }'
