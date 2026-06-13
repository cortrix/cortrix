#!/usr/bin/env bash
# GET /api/v1/tenants/{tenant_id}/quota — success
curl -X GET "https://api.cortrix.io/api/v1/tenants/{tenant_id}/quota" \
  -H "X-API-Key: cx_live_xxx"
