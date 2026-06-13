#!/usr/bin/env bash
# PUT /api/v1/system/agent_llm_config — success
curl -X PUT "https://api.cortrix.io/api/v1/system/agent_llm_config" \
  -H "X-API-Key: cx_live_xxx" \
  -H "Content-Type: application/json" \
  -d '{ /* TODO: request payload, see components/schemas */ }'
