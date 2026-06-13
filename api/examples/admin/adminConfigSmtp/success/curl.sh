#!/usr/bin/env bash
# POST /api/v1/admin/config/smtp — success
curl -X POST "https://api.cortrix.io/api/v1/admin/config/smtp" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
