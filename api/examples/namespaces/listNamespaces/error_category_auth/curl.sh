#!/usr/bin/env bash
# GET /api/v1/namespaces — error (HTTP 401)
curl -X GET "https://api.cortrix.io/api/v1/namespaces" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 401, see response.json
