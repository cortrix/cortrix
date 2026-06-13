#!/usr/bin/env bash
# POST /api/v1/maintenance/vacuum — error (HTTP 401)
curl -X POST "https://api.cortrix.io/api/v1/maintenance/vacuum" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 401, see response.json
