#!/usr/bin/env bash
# POST /api/v1/sql — error (HTTP 429)
curl -X POST "https://api.cortrix.io/api/v1/sql" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
# → HTTP 429, see response.json
