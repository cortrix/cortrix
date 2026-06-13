#!/usr/bin/env bash
# POST /api/v1/auth/password-reset — error (HTTP 503)
curl -X POST "https://api.cortrix.io/api/v1/auth/password-reset" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
# → HTTP 503, see response.json
