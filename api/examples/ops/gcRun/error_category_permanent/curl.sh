#!/usr/bin/env bash
# POST /api/v1/gc/run — error (HTTP 400)
curl -X POST "https://api.cortrix.io/api/v1/gc/run" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 400, see response.json
