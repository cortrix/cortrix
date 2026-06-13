#!/usr/bin/env bash
# POST /api/v1/gc/restore — success
curl -X POST "https://api.cortrix.io/api/v1/gc/restore" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
