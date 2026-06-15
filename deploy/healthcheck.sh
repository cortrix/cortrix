#!/bin/bash
# ==========================================================
# Cortrix Health Check
# Called by: Docker HEALTHCHECK, systemd, external monitors
#
# Exit codes:
#   0 — healthy (HTTP 200) or degraded-but-serving (HTTP 503)
#   1 — unreachable / error
# ==========================================================

PORT="${CORTRIX_HTTP_PORT:-8420}"
ENDPOINT="http://localhost:${PORT}/api/v1/health"
TIMEOUT=4  # seconds — must be < Docker HEALTHCHECK timeout (5s)

HTTP_CODE=$(curl -s --max-time "$TIMEOUT" -o /tmp/health_body -w "%{http_code}" "$ENDPOINT" 2>/dev/null)

case "$HTTP_CODE" in
    200)
        # All components healthy
        exit 0
        ;;
    503)
        # Degraded but still serving (e.g. model not loaded)
        echo "WARN: service degraded — $(cat /tmp/health_body 2>/dev/null)"
        exit 0
        ;;
    *)
        echo "FAIL: health check returned HTTP $HTTP_CODE"
        exit 1
        ;;
esac
