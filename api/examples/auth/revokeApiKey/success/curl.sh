#!/usr/bin/env bash
# DELETE /api/v1/auth/api-keys/{id} — success
curl -X DELETE "https://api.cortrix.io/api/v1/auth/api-keys/{id}" \
  -H "X-API-Key: cx_live_xxx"
