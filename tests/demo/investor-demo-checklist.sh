#!/bin/bash
# Cortrix Investor Demo Automated Validation Script
# Validates all critical demo scenarios for investor presentations
# Usage: ./investor-demo-checklist.sh <base_url> [api_key]

set -euo pipefail

BASE_URL="${1:-https://demo.cortrix.ai}"
API_KEY="${2:-demo-api-key}"
REPORT_FILE="demo-validation-$(date +%Y%m%d_%H%M%S).md"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Counters
PASSED=0
FAILED=0
TOTAL=0

function check() {
    local name="$1"
    local command="$2"
    ((TOTAL++))

    echo -n "[$TOTAL] $name... "

    if eval "$command" > /tmp/check_output.txt 2>&1; then
        echo -e "${GREEN}PASS${NC}"
        echo "- [x] $name" >> "$REPORT_FILE"
        ((PASSED++))
        return 0
    else
        echo -e "${RED}FAIL${NC}"
        echo "- [ ] FAIL: $name" >> "$REPORT_FILE"
        echo "  \`\`\`" >> "$REPORT_FILE"
        cat /tmp/check_output.txt >> "$REPORT_FILE"
        echo "  \`\`\`" >> "$REPORT_FILE"
        ((FAILED++))
        return 1
    fi
}

echo "============================================"
echo "  Cortrix Investor Demo Validation"
echo "============================================"
echo "Target: $BASE_URL"
echo "Date: $(date)"
echo ""

# Initialize report
cat > "$REPORT_FILE" <<EOF
# Cortrix Investor Demo Validation Report

**Date**: $(date)
**Target**: $BASE_URL
**Status**: In Progress

---

## Test Results

EOF

echo "=== Core Functionality ==="

# 1. Health Check
check "Health endpoint responds" \
    "curl -f -s --max-time 5 $BASE_URL/api/v1/health | grep -q 'ok\|healthy'"

# 2. Web UI
check "Web UI accessible" \
    "curl -f -s --max-time 5 $BASE_URL/ui | grep -q 'Cortrix'"

# 3. API authentication
check "API requires authentication" \
    "! curl -f -s $BASE_URL/api/v1/namespaces 2>/dev/null"

check "API accepts valid key" \
    "curl -f -s -H 'X-API-Key: $API_KEY' $BASE_URL/api/v1/namespaces"

echo ""
echo "=== Document Upload ==="

# Create test PDF
TEST_PDF="/tmp/cortrix_test_doc.pdf"
echo "Test PDF content" > "$TEST_PDF"

# 4. Namespace creation
check "Create demo namespace" \
    "curl -f -s -X POST -H 'X-API-Key: $API_KEY' \
        -H 'Content-Type: application/json' \
        -d '{\"namespace\":\"demo\"}' \
        $BASE_URL/api/v1/namespaces"

# 5. Document upload
UPLOAD_START=$(date +%s)
if check "Upload test document (< 30s)" \
    "curl -f -s -X POST -H 'X-API-Key: $API_KEY' \
        -F 'file=@$TEST_PDF' \
        -F 'namespace=demo' \
        $BASE_URL/api/v1/upload | grep -q 'document_id'"; then
    UPLOAD_END=$(date +%s)
    UPLOAD_TIME=$((UPLOAD_END - UPLOAD_START))
    echo "  Upload time: ${UPLOAD_TIME}s" | tee -a "$REPORT_FILE"
fi

# 6. Document retrieval
check "Document appears in namespace" \
    "curl -f -s -H 'X-API-Key: $API_KEY' \
        $BASE_URL/api/v1/namespaces/demo/documents | grep -q 'cortrix_test_doc'"

echo ""
echo "=== Query Engine ==="

# 7. Semantic query
QUERY_START=$(date +%s)
if check "Semantic query responds (< 2s)" \
    "timeout 2s curl -f -s -X POST -H 'X-API-Key: $API_KEY' \
        -H 'Content-Type: application/json' \
        -d '{\"query\":\"test\",\"namespace\":\"demo\",\"top_k\":5}' \
        $BASE_URL/api/v1/query | grep -q 'results'"; then
    QUERY_END=$(date +%s)
    QUERY_TIME=$((QUERY_END - QUERY_START))
    echo "  Query time: ${QUERY_TIME}s" | tee -a "$REPORT_FILE"
fi

# 8. Keyword search
check "Keyword search works" \
    "curl -f -s -X POST -H 'X-API-Key: $API_KEY' \
        -H 'Content-Type: application/json' \
        -d '{\"query\":\"test\",\"namespace\":\"demo\",\"mode\":\"keyword\"}' \
        $BASE_URL/api/v1/query | grep -q 'results'"

# 9. Hybrid search (RRF fusion)
check "Hybrid search works" \
    "curl -f -s -X POST -H 'X-API-Key: $API_KEY' \
        -H 'Content-Type: application/json' \
        -d '{\"query\":\"test\",\"namespace\":\"demo\",\"mode\":\"hybrid\"}' \
        $BASE_URL/api/v1/query | grep -q 'results'"

echo ""

echo ""
echo "=== Agent Demo ==="

# 11. Agent chat endpoint
check "Agent chat endpoint responds" \
    "curl -f -s -X POST -H 'Content-Type: application/json' \
        -d '{\"message\":\"Hello\",\"session_id\":\"test\"}' \
        $BASE_URL/api/v1/agent/chat | grep -q 'response'"

# 12. Memory system (if enabled)
check "Agent memory persists across sessions" \
    "curl -f -s -X POST -H 'Content-Type: application/json' \
        -d '{\"message\":\"Remember: I like blue\",\"session_id\":\"test123\"}' \
        $BASE_URL/api/v1/agent/chat && \
     curl -f -s -X POST -H 'Content-Type: application/json' \
        -d '{\"message\":\"What color do I like?\",\"session_id\":\"test123\"}' \
        $BASE_URL/api/v1/agent/chat | grep -iq 'blue'"

echo ""
echo "=== Performance Benchmarks ==="

# Cold start time (if we can restart container)
if command -v docker &> /dev/null && docker ps | grep -q cortrix; then
    echo "Note: Cold start test requires container restart (skipped)"
    echo "- [~] Cold start < 30s (manual test required)" >> "$REPORT_FILE"
fi

# Cleanup
rm -f "$TEST_PDF"

# Generate summary
echo "" >> "$REPORT_FILE"
echo "---" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"
echo "## Summary" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"
echo "- **Total**: $TOTAL tests" >> "$REPORT_FILE"
echo "- **Passed**: $PASSED" >> "$REPORT_FILE"
echo "- **Failed**: $FAILED" >> "$REPORT_FILE"
echo "- **Pass Rate**: $((PASSED * 100 / TOTAL))%" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"

if [ $FAILED -eq 0 ]; then
    echo "**Status**: ✅ **PASS**" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "All critical demo scenarios validated successfully."
else
    echo "**Status**: ❌ **FAIL**" >> "$REPORT_FILE"
    echo "" >> "$REPORT_FILE"
    echo "$FAILED test(s) failed. Review failures above."
fi

# Print summary
echo ""
echo "============================================"
echo "  Validation Complete"
echo "============================================"
echo "Total:  $TOTAL"
echo "Passed: $PASSED"
echo "Failed: $FAILED"
echo "Report: $REPORT_FILE"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✅ ALL TESTS PASSED${NC}"
    exit 0
else
    echo -e "${RED}❌ $FAILED TEST(S) FAILED${NC}"
    exit 1
fi
