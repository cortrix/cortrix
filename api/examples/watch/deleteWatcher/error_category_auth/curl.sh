#!/usr/bin/env bash
# DELETE /api/v1/watch/{id} — error (HTTP 401)
curl -X DELETE "https://api.cortrix.io/api/v1/watch/{id}" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 401, see response.json
