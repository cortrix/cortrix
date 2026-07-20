#!/bin/bash
set -u

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }
contains() { grep -Fq -- "$2" "$1"; }
not_contains() { ! grep -Fq -- "$2" "$1"; }

echo "Cortrix Docker readiness contract tests"

HEALTHCHECK="$DEPLOY_DIR/healthcheck.sh"
ENTRYPOINT="$DEPLOY_DIR/entrypoint.sh"
SUPERVISOR="$DEPLOY_DIR/supervisord.conf"
COMPOSE="$DEPLOY_DIR/docker-compose.yml"

if bash -n "$HEALTHCHECK" && bash -n "$ENTRYPOINT"; then
    pass "healthcheck and entrypoint syntax is valid"
else
    fail "healthcheck or entrypoint syntax is invalid"
fi

if contains "$HEALTHCHECK" '/api/v1/system/health/ready' && \
   contains "$HEALTHCHECK" "if [ \"\$HTTP_CODE\" = \"200\" ]"; then
    pass "healthcheck accepts only the strict readiness endpoint"
else
    fail "healthcheck does not enforce strict readiness"
fi
if not_contains "$HEALTHCHECK" '503)' && not_contains "$HEALTHCHECK" 'degraded-but-serving'; then
    pass "HTTP 503 is not treated as healthy"
else
    fail "healthcheck still treats HTTP 503 as healthy"
fi

for contract in 'CORTRIX_PROFILE:-quickstart' 'CORTRIX_LLM_ENABLED:-false' \
    'CORTRIX_AGENT_ENABLED:-false' 'CORTRIX_QUICKSTART_BOOTSTRAP_ENABLED:-true' \
    'real embedding/reranker asset is missing or unsafe' \
    "rm -f \"\$CORTRIX_QUICKSTART_READY_FILE\""; do
    if contains "$ENTRYPOINT" "$contract"; then
        pass "entrypoint contains $contract"
    else
        fail "entrypoint is missing $contract"
    fi
done

if contains "$ENTRYPOINT" 'CORTRIX_LLM_PROVIDER="disabled"' && \
   contains "$ENTRYPOINT" 'CORTRIX_LLM_API_KEY=""' && \
   contains "$COMPOSE" 'CORTRIX_LLM_ENABLED: "false"'; then
    pass "default QuickStart explicitly disables LLM configuration"
else
    fail "no-LLM default is incomplete"
fi

if contains "$SUPERVISOR" '/api/v1/system/health/live' && \
   not_contains "$SUPERVISOR" 'HTTP" != "503'; then
    pass "health monitor uses liveness instead of accepting degraded readiness"
else
    fail "health monitor contract is stale"
fi

TOTAL=$((PASS + FAIL))
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
