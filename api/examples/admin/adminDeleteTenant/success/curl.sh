#!/usr/bin/env bash
# DELETE /api/v1/admin/tenants/{id} — success
curl -X DELETE "https://api.cortrix.io/api/v1/admin/tenants/{id}" \
  -H "X-API-Key: cx_live_xxx"
