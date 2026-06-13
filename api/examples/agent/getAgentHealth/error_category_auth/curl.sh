#!/usr/bin/env bash
# GET /api/v1/agent/health — error (HTTP 403)
curl -X GET "https://api.cortrix.io/api/v1/agent/health" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 403, see response.json
