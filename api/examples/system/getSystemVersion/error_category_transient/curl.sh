#!/usr/bin/env bash
# GET /api/v1/system/version — error (HTTP 429)
curl -X GET "https://api.cortrix.io/api/v1/system/version" \
  -H "X-API-Key: cx_live_xxx"
# → HTTP 429, see response.json
