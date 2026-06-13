#!/usr/bin/env bash
# DELETE /api/v1/namespaces/{ns} — success
curl -X DELETE "https://api.cortrix.io/api/v1/namespaces/{ns}" \
  -H "X-API-Key: cx_live_xxx"
