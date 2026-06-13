#!/usr/bin/env bash
# DELETE /api/v1/namespaces/{ns} — error (HTTP 429)
curl -X DELETE "https://api.cortrix.io/api/v1/namespaces/{ns}" \
  -H "X-API-Key: cx_live_xxx"
# -> HTTP 429, see response.json
