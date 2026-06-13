#!/usr/bin/env bash
# POST /api/v1/sync/start — error (HTTP 400)
curl -X POST "https://api.cortrix.io/api/v1/sync/start" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
# → HTTP 400, see response.json
