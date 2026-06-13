#!/bin/bash
# ============================================================
# F12 Test: Health Check — verify deploy/ config correctness
# Run:  bash codes/cortrix/deploy/test_healthcheck.sh
# ============================================================

set -e

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

echo "========================================"
echo "  F12 Health Check Test Suite"
echo "========================================"
echo ""

# --- 1. healthcheck.sh content ---
echo "Test 1: healthcheck.sh"
HC="$DEPLOY_DIR/healthcheck.sh"

if bash -n "$HC" 2>/dev/null; then
    pass "syntax valid"
else
    fail "syntax error"
fi

if grep -q "curl" "$HC"; then
    pass "uses curl for HTTP check"
else
    fail "missing curl"
fi

if grep -q "CORTRIX_HTTP_PORT" "$HC"; then
    pass "respects CORTRIX_HTTP_PORT"
else
    fail "missing CORTRIX_HTTP_PORT"
fi

if grep -q "/api/v1/health" "$HC"; then
    pass "checks /api/v1/health endpoint"
else
    fail "wrong health endpoint"
fi

if grep -q "503" "$HC"; then
    pass "handles degraded (503) status"
else
    fail "missing 503 handling"
fi

if grep -q "exit 0" "$HC" && grep -q "exit 1" "$HC"; then
    pass "has healthy + unhealthy exit codes"
else
    fail "incomplete exit codes"
fi

# --- 2. entrypoint.sh pre-flight checks ---
echo ""
echo "Test 2: entrypoint.sh pre-flight checks"
EP="$DEPLOY_DIR/entrypoint.sh"

if bash -n "$EP" 2>/dev/null; then
    pass "syntax valid"
else
    fail "syntax error"
fi

if grep -q 'CORTRIX_DATA_DIR' "$EP"; then
    pass "sets CORTRIX_DATA_DIR default"
else
    fail "missing CORTRIX_DATA_DIR"
fi

if grep -q 'CORTRIX_HTTP_PORT' "$EP"; then
    pass "sets CORTRIX_HTTP_PORT default"
else
    fail "missing CORTRIX_HTTP_PORT"
fi

if grep -q 'CORTRIX_LLM_API_KEY' "$EP"; then
    pass "warns about missing API key"
else
    fail "missing API key check"
fi

if grep -q 'cortrix_server' "$EP"; then
    pass "checks cortrix_server binary"
else
    fail "missing binary check"
fi

if grep -q 'mkdir.*blobs' "$EP"; then
    pass "initializes data subdirectories"
else
    fail "missing data dir init"
fi

if grep -q 'supervisord' "$EP"; then
    pass "starts supervisord"
else
    fail "missing supervisord start"
fi

# --- 3. supervisord.conf process management ---
echo ""
echo "Test 3: supervisord.conf process management"
SUP="$DEPLOY_DIR/supervisord.conf"

if grep -q "nodaemon=true" "$SUP"; then
    pass "runs in foreground (nodaemon=true)"
else
    fail "missing nodaemon=true"
fi

if grep -q 'CORTRIX_LLM_API_KEY' "$SUP"; then
    pass "passes LLM API key to services"
else
    fail "missing LLM key passthrough"
fi

if grep -q 'startretries=3' "$SUP"; then
    pass "retry policy configured"
else
    fail "missing retry policy"
fi

if grep -q 'priority=' "$SUP"; then
    pass "start priority configured"
else
    fail "missing start priority"
fi

# --- 4. env passthrough ---
echo ""
echo "Test 4: env passthrough"

if grep -q 'env_file' "$DEPLOY_DIR/docker-compose.yml"; then
    pass "docker-compose passes env to container"
else
    fail "docker-compose missing env passthrough"
fi

# --- 5. Caddy HTTPS ---
echo ""
echo "Test 5: Caddy config"
CADDY="$DEPLOY_DIR/caddy/Caddyfile"

if grep -q "reverse_proxy localhost:8080" "$CADDY"; then
    pass "proxies to localhost:8080"
else
    fail "wrong proxy target"
fi

if grep -q "encode" "$CADDY"; then
    pass "compression enabled"
else
    fail "missing compression"
fi

if grep -q "X-Content-Type-Options" "$CADDY"; then
    pass "security headers present"
else
    fail "missing security headers"
fi

if grep -q "log" "$CADDY"; then
    pass "access logging configured"
else
    fail "missing access logging"
fi

# --- Summary ---
echo ""
echo "========================================"
TOTAL=$((PASS + FAIL))
echo "  Results: $PASS/$TOTAL passed, $FAIL failed"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
