#!/bin/bash
# ============================================================
# F12 Test: Docker Build — verify all deploy/ artifacts exist
# Run from any directory:  bash codes/cortrix/deploy/test_docker_build.sh
# ============================================================

set -e

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$DEPLOY_DIR")"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

echo "========================================"
echo "  F12 Docker Build Test Suite"
echo "  deploy/ dir: $DEPLOY_DIR"
echo "========================================"
echo ""

# --- 1. Required files in deploy/ ---
echo "Test 1: Required deploy/ files"
for f in Dockerfile supervisord.conf docker-compose.yml entrypoint.sh healthcheck.sh .env.example caddy/Caddyfile; do
    if [ -f "$DEPLOY_DIR/$f" ]; then
        pass "$f exists"
    else
        fail "$f MISSING"
    fi
done

# --- 2. Scripts are executable ---
echo ""
echo "Test 2: Script permissions"
for f in entrypoint.sh healthcheck.sh; do
    if [ -x "$DEPLOY_DIR/$f" ]; then
        pass "$f is executable"
    else
        fail "$f is NOT executable"
    fi
done

# --- 3. Shell syntax validation ---
echo ""
echo "Test 3: Shell script syntax (bash -n)"
for f in entrypoint.sh healthcheck.sh; do
    if bash -n "$DEPLOY_DIR/$f" 2>/dev/null; then
        pass "$f syntax OK"
    else
        fail "$f syntax ERROR"
    fi
done

# --- 4. Dockerfile content checks ---
echo ""
echo "Test 4: Dockerfile content"
DF="$DEPLOY_DIR/Dockerfile"

if grep -q "^FROM.*AS builder" "$DF"; then
    pass "Has builder stage"
else
    fail "Missing builder stage"
fi

if grep -q "^FROM.*AS web-builder" "$DF"; then
    pass "Has web-builder stage"
else
    fail "Missing web-builder stage"
fi

if grep -q "^FROM ubuntu:22.04$" "$DF"; then
    pass "Runtime stage uses ubuntu:22.04"
else
    fail "Runtime stage base image unexpected"
fi

if grep -q "HEALTHCHECK" "$DF"; then
    pass "HEALTHCHECK directive present"
else
    fail "Missing HEALTHCHECK directive"
fi

if grep -q "ENTRYPOINT" "$DF"; then
    pass "ENTRYPOINT directive present"
else
    fail "Missing ENTRYPOINT directive"
fi

if grep -q "VOLUME" "$DF"; then
    pass "VOLUME /data declared"
else
    fail "Missing VOLUME declaration"
fi

if grep -q "EXPOSE 8080" "$DF"; then
    pass "EXPOSE 8080 present"
else
    fail "Missing EXPOSE 8080"
fi

if grep -q "useradd.*cortrix" "$DF"; then
    pass "Non-root user cortrix created"
else
    fail "Missing non-root user"
fi

# --- 5. supervisord.conf structure ---
echo ""
echo "Test 5: supervisord.conf programs"
SUP="$DEPLOY_DIR/supervisord.conf"

if grep -q "\[program:cortrix-server\]" "$SUP"; then
    pass "cortrix-server program defined"
else
    fail "Missing cortrix-server"
fi

if grep -q "\[program:cortrix-agent\]" "$SUP"; then
    pass "cortrix-agent program defined"
else
    fail "Missing cortrix-agent"
fi

if grep -q "\[program:health-monitor\]" "$SUP"; then
    pass "health-monitor program defined"
else
    fail "Missing health-monitor"
fi

if grep -q "autorestart=true" "$SUP"; then
    pass "autorestart enabled"
else
    fail "autorestart missing"
fi

# --- 6. docker-compose.yml ---
echo ""
echo "Test 6: docker-compose.yml"
DC="$DEPLOY_DIR/docker-compose.yml"

if grep -q "cortrix:" "$DC"; then
    pass "cortrix service defined"
else
    fail "Missing cortrix service"
fi

if grep -q "postgres:" "$DC"; then
    pass "postgres service defined"
else
    fail "Missing postgres service"
fi

if grep -q "healthcheck:" "$DC"; then
    pass "healthcheck defined"
else
    fail "Missing healthcheck"
fi

# --- 7. .env.example completeness ---
echo ""
echo "Test 7: .env.example completeness"
ENV="$DEPLOY_DIR/.env.example"

for var in CORTRIX_DATA_DIR CORTRIX_HTTP_PORT CORTRIX_LLM_API_KEY CORTRIX_LLM_PROVIDER CORTRIX_ENABLE_MEMORY CORTRIX_AGENT_PORT; do
    if grep -q "$var" "$ENV"; then
        pass "$var documented"
    else
        fail "$var MISSING from .env.example"
    fi
done

# --- 8. Caddy reverse proxy ---
echo ""
echo "Test 8: Caddyfile"
CADDY="$DEPLOY_DIR/caddy/Caddyfile"

if grep -q "reverse_proxy" "$CADDY"; then
    pass "reverse_proxy directive present"
else
    fail "Missing reverse_proxy"
fi

if grep -q "Strict-Transport-Security" "$CADDY"; then
    pass "HSTS header present"
else
    fail "Missing HSTS header"
fi

# --- 9. Docker build (optional) ---
echo ""
echo "Test 9: Docker build (requires Docker daemon)"
if command -v docker &> /dev/null && docker info &> /dev/null 2>&1; then
    CONTEXT_DIR="$(dirname "$(dirname "$PROJECT_DIR")")"  # codes/ dir
    echo "  Build context: $CONTEXT_DIR"
    echo "  Dockerfile:    $DEPLOY_DIR/Dockerfile"
    if docker build -t "cortrix:build-test" -f "$DEPLOY_DIR/Dockerfile" "$CONTEXT_DIR" 2>&1 | tail -5; then
        pass "Docker build succeeded"
        # Check size
        SIZE_B=$(docker image inspect "cortrix:build-test" --format='{{.Size}}' 2>/dev/null || echo 0)
        SIZE_MB=$((SIZE_B / 1024 / 1024))
        echo "  Image size: ${SIZE_MB} MB"
        if [ "$SIZE_MB" -lt 3072 ]; then
            pass "Image < 3GB target"
        else
            fail "Image exceeds 3GB"
        fi
        docker rmi "cortrix:build-test" 2>/dev/null || true
    else
        fail "Docker build failed (expected during early MVP — code integration pending)"
    fi
else
    echo "  [SKIP] Docker not available"
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
