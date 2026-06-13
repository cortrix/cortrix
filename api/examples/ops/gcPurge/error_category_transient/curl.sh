#!/usr/bin/env bash
# POST /api/v1/gc/purge — error (HTTP 429)
curl -X POST "https://api.cortrix.io/api/v1/gc/purge" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 429, see response.json
