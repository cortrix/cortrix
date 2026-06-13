#!/usr/bin/env bash
# POST /api/v1/gc/purge — error (HTTP 400)
curl -X POST "https://api.cortrix.io/api/v1/gc/purge" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 400, see response.json
