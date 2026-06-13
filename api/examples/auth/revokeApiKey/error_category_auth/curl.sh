#!/usr/bin/env bash
# DELETE /api/v1/auth/api-keys/{id} — error (HTTP 401)
curl -X DELETE "https://api.cortrix.io/api/v1/auth/api-keys/{id}" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 401, see response.json
