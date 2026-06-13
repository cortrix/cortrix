#!/usr/bin/env bash
# POST /api/v1/sync/stop — error (HTTP 404)
curl -X POST "https://api.cortrix.io/api/v1/sync/stop" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
# → HTTP 404, see response.json
