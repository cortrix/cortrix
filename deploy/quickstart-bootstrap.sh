#!/bin/bash
set -euo pipefail

PORT="${CORTRIX_HTTP_PORT:-8420}"
METRICS_PORT="${CORTRIX_METRICS_PORT:-9091}"
BASE_URL="http://127.0.0.1:${PORT}"
METRICS_URL="http://127.0.0.1:${METRICS_PORT}/metrics"
READY_FILE="${CORTRIX_QUICKSTART_READY_FILE:-/data/quickstart/ready.json}"
FIXTURE="/app/fixtures/quickstart-demo.txt"
NAMESPACE="demo"
EXPECTED_TEXT="semantic storage keeps durable context close to the agents that need it"
STATE_DIR="$(dirname "$READY_FILE")"

mkdir -p "$STATE_DIR"
WORK_DIR="$(mktemp -d "$STATE_DIR/.bootstrap.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

fail() {
    echo "ERROR: QuickStart bootstrap: $*" >&2
    exit 1
}

json_field() {
    python3 - "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    document = json.load(stream)
value = document.get(sys.argv[2], "")
if isinstance(value, (dict, list)):
    print(json.dumps(value, separators=(",", ":")))
else:
    print(value)
PY
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

echo "[quickstart] waiting for the Cortrix API"
LIVE=0
for _attempt in $(seq 1 180); do
    if curl --fail --silent --show-error --max-time 4 \
        "$BASE_URL/api/v1/system/health/live" >/dev/null 2>&1; then
        LIVE=1
        break
    fi
    sleep 2
done
[ "$LIVE" -eq 1 ] || fail "server liveness did not become available within 360 seconds"

echo "[quickstart] creating the source-backed demo namespace"
HTTP_CODE="$(curl --silent --show-error --max-time 10 \
    --output "$WORK_DIR/namespace.json" --write-out '%{http_code}' \
    --header 'Content-Type: application/json' \
    --data '{"name":"demo"}' \
    "$BASE_URL/api/v1/namespaces")"
case "$HTTP_CODE" in
    200|201|409) ;;
    *) fail "namespace creation returned HTTP $HTTP_CODE" ;;
esac

echo "[quickstart] ingesting the bundled demo fixture"
HTTP_CODE="$(curl --silent --show-error --max-time 30 \
    --output "$WORK_DIR/upload.json" --write-out '%{http_code}' \
    --form "file=@${FIXTURE}" \
    "$BASE_URL/api/v1/namespaces/${NAMESPACE}/documents")"
case "$HTTP_CODE" in
    200|201) ;;
    *) fail "demo upload returned HTTP $HTTP_CODE" ;;
esac
DOC_ID="$(json_field "$WORK_DIR/upload.json" doc_id)"
[ -n "$DOC_ID" ] || fail "demo upload response did not contain doc_id"

echo "[quickstart] waiting for real embedding ingestion"
DOCUMENT_READY=0
for _attempt in $(seq 1 180); do
    curl --fail --silent --show-error --max-time 10 \
        --output "$WORK_DIR/status.json" \
        "$BASE_URL/api/v1/namespaces/${NAMESPACE}/documents/${DOC_ID}/status" \
        || fail "document status request failed"
    DOCUMENT_STATUS="$(json_field "$WORK_DIR/status.json" status)"
    case "$DOCUMENT_STATUS" in
        ready)
            DOCUMENT_READY=1
            break
            ;;
        error|failed|cancelled)
            fail "document processing entered terminal status: $DOCUMENT_STATUS"
            ;;
    esac
    sleep 2
done
[ "$DOCUMENT_READY" -eq 1 ] || fail "demo document did not become ready within 360 seconds"

echo "[quickstart] executing a real reranked retrieval"
curl --fail --silent --show-error --max-time 120 \
    --output "$WORK_DIR/query.json" \
    --header 'Content-Type: application/json' \
    --data '{"namespaces":["demo"],"query":"What does semantic storage keep close to the agents that need it?","top_k":5,"rerank":true}' \
    "$BASE_URL/api/v1/query" \
    || fail "reranked demo query failed"

python3 - "$WORK_DIR/query.json" "$EXPECTED_TEXT" <<'PY' || \
    fail "query response was not source-backed or did not contain a rerank score"
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    response = json.load(stream)
results = response.get("results") or []
expected = sys.argv[2].casefold()
valid = any(
    isinstance(item.get("rerank_score"), (int, float))
    and item.get("metadata", {}).get("source_path") == "quickstart-demo.txt"
    and expected in item.get("content", "").casefold()
    for item in results
)
if not valid:
    raise SystemExit(1)
PY

curl --fail --silent --show-error --max-time 10 \
    --output "$WORK_DIR/metrics.txt" "$METRICS_URL" \
    || fail "metrics endpoint was unavailable"

METRIC_COUNTS="$(python3 - "$WORK_DIR/metrics.txt" <<'PY'
import sys

metrics = {}
with open(sys.argv[1], encoding="utf-8") as stream:
    for raw_line in stream:
        line = raw_line.strip()
        if not line or line.startswith("#") or " " not in line:
            continue
        name, value = line.rsplit(" ", 1)
        metrics[name] = value
embedding = int(float(metrics.get("cortrix_onnx_inference_duration_seconds_count", "0")))
reranker = int(float(metrics.get("cortrix_reranker_score_duration_seconds_count", "0")))
embedding_active = float(metrics.get('cortrix_onnx_embedding_active_ep{ep="cpu"}', "0"))
reranker_active = float(metrics.get('cortrix_reranker_active_ep{ep="cpu"}', "0"))
if embedding < 1 or reranker < 1 or embedding_active != 1 or reranker_active != 1:
    raise SystemExit(1)
print(embedding, reranker)
PY
)" || fail "real embedding/reranker invocation metrics were not present"
read -r EMBEDDING_CALLS RERANKER_CALLS <<< "$METRIC_COUNTS"
[ "$EMBEDDING_CALLS" -ge 1 ] || fail "embedding invocation counter is zero"
[ "$RERANKER_CALLS" -ge 1 ] || fail "reranker invocation counter is zero"

EMBEDDING_SHA="$(sha256_file /data/models/bge-m3/model.onnx)"
RERANKER_SHA="$(sha256_file /data/models/bge-reranker-v2-m3/model.onnx)"
FIXTURE_SHA="$(sha256_file "$FIXTURE")"
SOURCE_REVISION="${CORTRIX_SOURCE_REVISION:-local-source}"
READY_TMP="$STATE_DIR/.ready.$$.tmp"
python3 - "$READY_TMP" "$SOURCE_REVISION" "$NAMESPACE" "$DOC_ID" \
    "$EMBEDDING_CALLS" "$RERANKER_CALLS" "$EMBEDDING_SHA" \
    "$RERANKER_SHA" "$FIXTURE_SHA" <<'PY'
import json
import sys

(
    output_path,
    source_revision,
    namespace,
    doc_id,
    embedding_calls,
    reranker_calls,
    embedding_sha256,
    reranker_sha256,
    fixture_sha256,
) = sys.argv[1:]
proof = {
    "status": "ready",
    "source_revision": source_revision,
    "namespace": namespace,
    "doc_id": doc_id,
    "embedding_calls": int(embedding_calls),
    "reranker_calls": int(reranker_calls),
    "embedding_sha256": embedding_sha256,
    "reranker_sha256": reranker_sha256,
    "fixture_sha256": fixture_sha256,
}
with open(output_path, "w", encoding="utf-8") as stream:
    json.dump(proof, stream, separators=(",", ":"), sort_keys=True)
    stream.write("\n")
PY
chmod 0644 "$READY_TMP"
mv -f "$READY_TMP" "$READY_FILE"

QUERY_TMP="$STATE_DIR/.query.$$.tmp"
cp "$WORK_DIR/query.json" "$QUERY_TMP"
chmod 0644 "$QUERY_TMP"
mv -f "$QUERY_TMP" "$STATE_DIR/query.json"

echo "[quickstart] ready: source-backed demo, real embedding, and real reranker verified"
