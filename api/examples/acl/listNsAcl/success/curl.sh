#!/usr/bin/env bash
# GET /api/v1/namespaces/{ns_id}/acl — success
curl -X GET "https://api.cortrix.io/api/v1/namespaces/{ns_id}/acl" \
  -H "X-API-Key: cx_live_xxx"
