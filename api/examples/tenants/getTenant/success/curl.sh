#!/usr/bin/env bash
# GET /api/v1/tenants/{tenant_id} — success
curl -X GET "https://api.cortrix.io/api/v1/tenants/{tenant_id}" \
  -H "X-API-Key: cx_live_xxx"
