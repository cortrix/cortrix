#!/usr/bin/env bash
# GET /api/v1/tenants/{tenant_id}/members — success
curl -X GET "https://api.cortrix.io/api/v1/tenants/{tenant_id}/members" \
  -H "X-API-Key: cx_live_xxx"
