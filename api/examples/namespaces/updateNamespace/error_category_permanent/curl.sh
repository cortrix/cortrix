#!/usr/bin/env bash
# PATCH /api/v1/namespaces/{ns} — error (HTTP 400)
curl -X PATCH "https://api.cortrix.io/api/v1/namespaces/{ns}" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
# -> HTTP 400, see response.json
