#!/bin/bash
set -u

PORT="${CORTRIX_HTTP_PORT:-8420}"
ENDPOINT="http://127.0.0.1:${PORT}/api/v1/system/health/ready"
TIMEOUT=4
BODY_FILE="$(mktemp /tmp/cortrix-health.XXXXXX)"
trap 'rm -f "$BODY_FILE"' EXIT INT TERM HUP

HTTP_CODE="$(curl --silent --show-error --max-time "$TIMEOUT" \
    --output "$BODY_FILE" --write-out '%{http_code}' "$ENDPOINT" 2>/dev/null || true)"

if [ "$HTTP_CODE" = "200" ]; then
    exit 0
fi

echo "FAIL: readiness check returned HTTP ${HTTP_CODE:-000}"
if [ -s "$BODY_FILE" ]; then
    cat "$BODY_FILE"
fi
exit 1
