#!/usr/bin/env bash
# Shows how a Cortrix query reports partial success, using only the Quick Start's
# built-in `demo` namespace. Run it after the Quick Start reports ready.
#
# Usage: ./check.sh [BASE_URL]        (default: http://127.0.0.1:8420)
set -euo pipefail

BASE_URL="${1:-${CORTRIX_URL:-http://127.0.0.1:8420}}"
BASE_URL="${BASE_URL%/}"   # a trailing slash would make the readiness probe 404 and exit 2
QUERY="${CORTRIX_QUERY:-What does semantic storage keep close to the agents that need it?}"
failures=0

if ! curl -fsS --max-time 10 "${BASE_URL}/api/v1/system/health/ready" >/dev/null 2>&1; then
    echo "Cortrix is not ready at ${BASE_URL}." >&2
    echo "Start the Quick Start first, or pass the base URL as the first argument." >&2
    exit 2
fi

# query <json-body>: prints the HTTP status on the first line and the response body after it.
# A transport failure (timeout, refused connection) prints status 000 and an empty body, so
# summarize reports a FAIL line instead of aborting the whole run under `set -e`.
query() {
    local raw
    raw="$(curl -sS --max-time 120 -H 'Content-Type: application/json' -d "$1" \
        -w '\n%{http_code}' "${BASE_URL}/api/v1/query" || true)"
    printf '%s' "$raw" | python3 -c '
import sys
body, _, status = sys.stdin.read().rpartition("\n")
print(status.strip() or "000")
print(body)'
}

# summarize: reads the output of `query` and prints one line with the fields an agent acts on.
summarize() {
    python3 -c '
import json, sys
status = sys.stdin.readline().strip()
raw = sys.stdin.read()
try:
    doc = json.loads(raw)
except ValueError:
    print("http=%s not_json=true" % status)
    raise SystemExit(0)
if "meta" not in doc:
    err = doc["error"]
    print("http=%s code=%s retryable=%s category=%s" % (
        status, err["code"], json.dumps(err["retryable"]), err["category"]))
else:
    meta = doc["meta"]
    failed = ",".join("%s(%s retryable=%s category=%s)" % (
        f["namespace"], f["error_code"], json.dumps(f["retryable"]), f["category"])
        for f in meta["namespaces_failed"]) or "-"
    print("http=%s coverage_ratio=%s results=%d succeeded=%s failed_ns=%s failed=%s%s" % (
        status, meta["coverage_ratio"], len(doc["results"]),
        ",".join(meta["namespaces_succeeded"]) or "-",
        ",".join(f["namespace"] for f in meta["namespaces_failed"]) or "-", failed,
        "" if "error" not in doc else " error=" + doc["error"]["code"]))'
}

# check <title> <json-body> <expected-prefix>
check() {
    local title="$1" body="$2" expected="$3" line
    line="$(query "$body" | summarize)"
    echo "== $title"
    echo "   $line"
    if [[ "$line" == "$expected"* ]]; then
        echo "   PASS"
    else
        echo "   FAIL: expected the line to start with: $expected"
        failures=$((failures + 1))
    fi
}

body() { python3 -c 'import json, sys; print(json.dumps({sys.argv[1]: json.loads(sys.argv[2]), "query": sys.argv[3], "top_k": 1}))' "$@"; }

check "1. Every namespace answers" \
    "$(body namespaces '["demo"]' "$QUERY")" \
    "http=200 coverage_ratio=1.0 "

check "2. One namespace cannot be queried: still HTTP 200, coverage_ratio drops" \
    "$(body namespaces '["demo", "nope"]' "$QUERY")" \
    "http=200 coverage_ratio=0.5 results=1 succeeded=demo failed_ns=nope failed=nope("

check "3. No namespace can be queried: still HTTP 200, coverage_ratio is 0 and results are empty" \
    "$(body namespaces '["nope", "nope-2"]' "$QUERY")" \
    "http=200 coverage_ratio=0.0 results=0 succeeded=- failed_ns=nope,nope-2 failed=nope("

check "4. A malformed request is different: it gets a 4xx, not a partial result" \
    "$(body namespace '"demo"' "$QUERY")" \
    "http=400 code=CX_ERR_DEPRECATED_FIELD "

echo
if [ "$failures" -eq 0 ]; then
    echo "All 4 checks passed."
else
    echo "$failures check(s) failed."
    exit 1
fi
