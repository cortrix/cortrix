#!/bin/bash
set -u

DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$DEPLOY_DIR")"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }
contains() { grep -Fq -- "$2" "$1"; }
not_contains() { ! grep -Fq -- "$2" "$1"; }

echo "Cortrix Docker QuickStart contract tests"

echo "Test 1: required files and shell syntax"
for file in Dockerfile docker-compose.yml cortrix.yaml supervisord.conf \
    entrypoint.sh healthcheck.sh download-models.sh quickstart-bootstrap.sh \
    model-manifest.tsv fixtures/quickstart-demo.txt; do
    if [ -f "$DEPLOY_DIR/$file" ]; then
        pass "$file exists"
    else
        fail "$file is missing"
    fi
done
for file in entrypoint.sh healthcheck.sh download-models.sh quickstart-bootstrap.sh; do
    if bash -n "$DEPLOY_DIR/$file"; then
        pass "$file syntax is valid"
    else
        fail "$file syntax is invalid"
    fi
done

echo "Test 2: pinned model supply contract"
MANIFEST="$DEPLOY_DIR/model-manifest.tsv"
ROW_COUNT="$(awk -F '\t' 'NR > 1 && NF {count++} END {print count + 0}' "$MANIFEST")"
if [ "$ROW_COUNT" -eq 4 ]; then
    pass "manifest contains exactly four required assets"
else
    fail "manifest must contain exactly four assets (found $ROW_COUNT)"
fi
if awk -F '\t' 'NR == 1 {next} length($4) != 40 || length($7) != 64 || $6 !~ /^[0-9]+$/ {exit 1}' "$MANIFEST"; then
    pass "manifest revisions, sizes, and SHA-256 fields are immutable"
else
    fail "manifest contains malformed identity fields"
fi
if not_contains "$MANIFEST" $'\tmain\t' && not_contains "$MANIFEST" $'\tlatest\t'; then
    pass "manifest contains no mutable main/latest revision"
else
    fail "manifest uses a mutable revision"
fi
DOWNLOADER="$DEPLOY_DIR/download-models.sh"
for contract in "--proto '=https'" "--proto-redir '=https'" "--retry-all-errors" \
    "expected_sha" "mktemp" "mv -f" "refusing symlink"; do
    if contains "$DOWNLOADER" "$contract"; then
        pass "downloader enforces $contract"
    else
        fail "downloader is missing $contract"
    fi
done
if "$DOWNLOADER" --self-test; then
    pass "checksum/size/symlink failure-path self-test passes"
else
    fail "model verification self-test failed"
fi

echo "Test 3: Compose zero-config and exposure contract"
COMPOSE="$DEPLOY_DIR/docker-compose.yml"
for contract in 'CORTRIX_PROFILE: quickstart' 'CORTRIX_LLM_ENABLED: "false"' \
    'CORTRIX_AGENT_ENABLED: "false"' 'CORTRIX_QUICKSTART_BOOTSTRAP_ENABLED: "true"' \
    'CORTRIX_SERVER_ALLOW_UNAUTHENTICATED_CONTAINER_BIND: "true"' \
    "127.0.0.1:\${CORTRIX_HTTP_PORT:-8420}:8420" 'CORTRIX_SOURCE_REVISION:'; do
    if contains "$COMPOSE" "$contract"; then
        pass "Compose contains $contract"
    else
        fail "Compose is missing $contract"
    fi
done
for forbidden in 'env_file:' 'container_name:' 'cortrix:latest' '0.0.0.0:'; do
    if not_contains "$COMPOSE" "$forbidden"; then
        pass "Compose excludes $forbidden"
    else
        fail "Compose must exclude $forbidden"
    fi
done

if command -v docker-compose >/dev/null 2>&1; then
    if env CORTRIX_SOURCE_REVISION=test-revision CORTRIX_HTTP_PORT=8420 \
        docker-compose -f "$COMPOSE" config >/dev/null; then
        pass "docker-compose config succeeds without deploy/.env"
    else
        fail "docker-compose config failed without deploy/.env"
    fi
elif command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    if env CORTRIX_SOURCE_REVISION=test-revision CORTRIX_HTTP_PORT=8420 \
        docker compose -f "$COMPOSE" config >/dev/null; then
        pass "docker compose config succeeds without deploy/.env"
    else
        fail "docker compose config failed without deploy/.env"
    fi
else
    echo "  [SKIP] Docker Compose is not installed"
fi

echo "Test 4: runtime proof and no-stub contract"
BOOTSTRAP="$DEPLOY_DIR/quickstart-bootstrap.sh"
SUPERVISOR="$DEPLOY_DIR/supervisord.conf"
CORE_BOOTSTRAP="$PROJECT_DIR/src/server/bootstrap.cpp"
for contract in '"rerank":true' 'cortrix_onnx_inference_duration_seconds_count' \
    'cortrix_reranker_score_duration_seconds_count' 'quickstart-demo.txt' \
    "mv -f \"\$READY_TMP\" \"\$READY_FILE\""; do
    if contains "$BOOTSTRAP" "$contract"; then
        pass "bootstrap contains $contract"
    else
        fail "bootstrap is missing $contract"
    fi
done
if contains "$SUPERVISOR" 'autostart=%(ENV_CORTRIX_AGENT_ENABLED)s' && \
   contains "$SUPERVISOR" '[program:quickstart-bootstrap]' && \
   contains "$SUPERVISOR" 'autostart=%(ENV_CORTRIX_QUICKSTART_BOOTSTRAP_ENABLED)s' && \
   contains "$SUPERVISOR" 'supervisorctl -c /etc/supervisor/conf.d/cortrix.conf restart cortrix-server'; then
    pass "Supervisor process gates and control socket are configured"
else
    fail "Supervisor process gates or control socket are incomplete"
fi
if contains "$CORE_BOOTSTRAP" 'CORTRIX_QUICKSTART_READY_FILE' && \
   contains "$CORE_BOOTSTRAP" 'source_backed_demo_pending'; then
    pass "Core readiness waits for source-backed QuickStart proof"
else
    fail "Core QuickStart readiness component is missing"
fi

echo "Test 5: Dockerfile identity and artifact contract"
DOCKERFILE="$DEPLOY_DIR/Dockerfile"
for contract in 'org.opencontainers.image.revision' 'model-manifest.tsv' \
    'quickstart-bootstrap.sh' 'quickstart-demo.txt' 'USER cortrix' \
    'HEALTHCHECK' 'CMake configure failed after'; do
    if contains "$DOCKERFILE" "$contract"; then
        pass "Dockerfile contains $contract"
    else
        fail "Dockerfile is missing $contract"
    fi
done

if [ "${CORTRIX_TEST_DOCKER_BUILD:-0}" = "1" ]; then
    if docker build --build-arg CORTRIX_SOURCE_REVISION=test-revision \
        -t cortrix:quickstart-contract-test -f "$DOCKERFILE" "$PROJECT_DIR"; then
        pass "Docker image build succeeds"
    else
        fail "Docker image build failed"
    fi
else
    echo "  [SKIP] Docker image build (set CORTRIX_TEST_DOCKER_BUILD=1)"
fi

TOTAL=$((PASS + FAIL))
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
