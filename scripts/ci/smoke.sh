#!/usr/bin/env bash
# =============================================================
# D-R3 #479: docker full-stack smoke (F23 E2E entry tier)
# Boots deploy/docker-compose.yml and walks the agent-facing
# golden path against the real all-in-one image:
#
#   health → ready(structure) → create ns → upload doc →
#   poll status=ready → query hit → metrics scrape →
#   graceful SIGTERM (clean exit) → restart → health again
#
# Usage:  scripts/ci/smoke.sh [--keep]   (--keep: leave stack up)
# Exit:   0 = all PASS, 1 = first failing assertion (verbose)
# =============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# plugin form (docker compose) on CI; standalone binary (docker-compose) locally
if docker compose version >/dev/null 2>&1; then
  COMPOSE="docker compose -f $ROOT/deploy/docker-compose.yml"
else
  COMPOSE="docker-compose -f $ROOT/deploy/docker-compose.yml"
fi
BASE="http://localhost:${CORTRIX_HTTP_PORT:-8420}"
NS="smoke"
KEEP="${1:-}"

PASS=0; FAIL=0
pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
die()  { fail "$1"; echo ""; echo "── container log tail ──"; docker logs cortrix 2>&1 | tail -40; [ "$KEEP" = "--keep" ] || $COMPOSE down -v >/dev/null 2>&1; exit 1; }

echo "── 0. compose up"
$COMPOSE up -d || die "compose up"

echo "── 1. wait for health (≤180s: first boot initializes /data)"
ok=""
for i in $(seq 1 60); do
  code=$(curl -s -o /tmp/smoke_health -w "%{http_code}" "$BASE/api/v1/health" 2>/dev/null)
  if [ "$code" = "200" ] || [ "$code" = "503" ]; then ok=1; break; fi
  sleep 3
done
[ -n "$ok" ] && pass "health reachable (HTTP $code)" || die "health unreachable after 180s"

echo "── 2. /ready structure (F20 ReadinessRegistry via F24 route)"
curl -s "$BASE/api/v1/system/health/ready" > /tmp/smoke_ready
python3 - <<'EOF' || die "/ready structure"
import json
d = json.load(open("/tmp/smoke_ready"))
assert "status" in d, "missing status"
assert "components" in d and isinstance(d["components"], dict), "missing components"
assert "disk" in d["components"], "disk probe not registered"
print(f"  [PASS] /ready: status={d['status']} components={list(d['components'])}")
EOF

echo "── 3. create namespace"
code=$(curl -s -o /tmp/smoke_ns -w "%{http_code}" -X POST "$BASE/api/v1/namespaces" \
  -H 'Content-Type: application/json' -d "{\"name\":\"$NS\"}")
{ [ "$code" = "200" ] || [ "$code" = "201" ] || [ "$code" = "409" ]; } \
  && pass "namespace create (HTTP $code)" || die "namespace create HTTP $code: $(cat /tmp/smoke_ns)"

echo "── 4. upload document"
cat > /tmp/smoke_doc.txt <<'EOF'
Cortrix is a semantic storage engine for the agentic era. The P-HNSW vector
index pairs with BM25 metadata search; the SPC pipeline chunks, enriches and
embeds every document so agents can query meaning, not just bytes.
EOF
code=$(curl -s -o /tmp/smoke_up -w "%{http_code}" -X POST \
  "$BASE/api/v1/namespaces/$NS/documents" -F "file=@/tmp/smoke_doc.txt")
[ "$code" = "200" ] || [ "$code" = "201" ] || [ "$code" = "202" ] \
  || die "upload HTTP $code: $(cat /tmp/smoke_up)"
DOC_ID=$(python3 -c "import json;d=json.load(open('/tmp/smoke_up'));print(d.get('doc_id') or d.get('id') or '')")
[ -n "$DOC_ID" ] && pass "upload accepted (HTTP $code, doc_id=$DOC_ID)" || die "no doc_id in: $(cat /tmp/smoke_up)"

echo "── 5. poll document status → ready (≤90s)"
ok=""
for i in $(seq 1 30); do
  st=$(curl -s "$BASE/api/v1/namespaces/$NS/documents/$DOC_ID/status" \
    | python3 -c "import json,sys;print(json.load(sys.stdin).get('status',''))" 2>/dev/null)
  if [ "$st" = "ready" ]; then ok=1; break; fi
  if [ "$st" = "error" ]; then die "doc entered error state"; fi
  sleep 3
done
[ -n "$ok" ] && pass "doc status=ready" || die "doc not ready after 90s (last: $st)"

echo "── 6. query golden path"
curl -s -X POST "$BASE/api/v1/query" -H 'Content-Type: application/json' \
  -d "{\"query\":\"what is cortrix\",\"namespace\":\"$NS\",\"top_k\":5}" > /tmp/smoke_q
python3 - <<'EOF' || die "query response"
import json
d = json.load(open("/tmp/smoke_q"))
results = d.get("results") or d.get("hits") or []
assert isinstance(results, list) and len(results) > 0, f"no results: {json.dumps(d)[:300]}"
print(f"  [PASS] query returned {len(results)} result(s)")
EOF

echo "── 7. metrics scrape (:9091 inside container)"
if docker exec cortrix curl -sf http://localhost:9091/metrics 2>/dev/null | grep -q "^cortrix_"; then
  pass "OpenMetrics exposes cortrix_* series"
else
  fail "metrics scrape (non-blocking: gauges may need traffic)"
fi

echo "── 8. graceful shutdown (SIGTERM via docker stop)"
docker stop -t 20 cortrix >/dev/null || die "docker stop"
exit_code=$(docker inspect cortrix --format '{{.State.ExitCode}}')
[ "$exit_code" = "0" ] && pass "clean exit (code 0)" || fail "exit code $exit_code (supervisord propagation — inspect logs)"

echo "── 9. restart → health recovers (resume-on-startup path)"
$COMPOSE up -d >/dev/null || die "compose re-up"
ok=""
for i in $(seq 1 40); do
  code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/v1/health" 2>/dev/null)
  if [ "$code" = "200" ] || [ "$code" = "503" ]; then ok=1; break; fi
  sleep 3
done
[ -n "$ok" ] && pass "health after restart (HTTP $code)" || die "no recovery after restart"

[ "$KEEP" = "--keep" ] || { echo "── teardown"; $COMPOSE down -v >/dev/null 2>&1; }

echo ""
echo "================ SMOKE: $PASS PASS / $FAIL FAIL ================"
[ "$FAIL" -eq 0 ]
