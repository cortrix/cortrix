#!/bin/bash
# Cortrix 24-hour stability monitoring script
# Usage: ./monitor-24h.sh [interval_seconds]
# Default: Check every 60 seconds for 24 hours

set -euo pipefail

INTERVAL="${1:-60}"
DURATION_HOURS=24
TOTAL_CHECKS=$((DURATION_HOURS * 3600 / INTERVAL))
LOG_FILE="stability-$(date +%Y%m%d_%H%M%S).log"
ALERT_FILE="alerts-$(date +%Y%m%d_%H%M%S).log"

echo "============================================" | tee -a "$LOG_FILE"
echo "  Cortrix 24h Stability Monitoring" | tee -a "$LOG_FILE"
echo "============================================" | tee -a "$LOG_FILE"
echo "Start time: $(date)" | tee -a "$LOG_FILE"
echo "Interval: ${INTERVAL}s" | tee -a "$LOG_FILE"
echo "Total checks: $TOTAL_CHECKS" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

# Counters
HEALTHY_COUNT=0
UNHEALTHY_COUNT=0
TOTAL_RESPONSE_TIME=0

for i in $(seq 1 "$TOTAL_CHECKS"); do
    TIMESTAMP=$(date +"%Y-%m-%d %H:%M:%S")

    # Health check
    HEALTH_START=$(date +%s.%N)
    if HEALTH=$(curl -s --max-time 5 http://localhost:8080/api/v1/health 2>&1); then
        HEALTH_END=$(date +%s.%N)
        RESPONSE_TIME=$(echo "$HEALTH_END - $HEALTH_START" | bc)

        STATUS=$(echo "$HEALTH" | jq -r '.status // "unknown"' 2>/dev/null || echo "unknown")

        if [ "$STATUS" = "ok" ] || [ "$STATUS" = "healthy" ]; then
            ((HEALTHY_COUNT++))
        else
            ((UNHEALTHY_COUNT++))
            echo "[$TIMESTAMP] ALERT: Unhealthy status: $STATUS" | tee -a "$ALERT_FILE"
        fi

        # Memory usage
        MEM_USAGE=$(docker stats cortrix --no-stream --format "{{.MemUsage}}" 2>/dev/null || echo "N/A")

        # CPU usage
        CPU_USAGE=$(docker stats cortrix --no-stream --format "{{.CPUPerc}}" 2>/dev/null || echo "N/A")

        # Log entry
        echo "[$TIMESTAMP] #$i/$TOTAL_CHECKS | Status: $STATUS | Memory: $MEM_USAGE | CPU: $CPU_USAGE | Response: ${RESPONSE_TIME}s" | tee -a "$LOG_FILE"

        TOTAL_RESPONSE_TIME=$(echo "$TOTAL_RESPONSE_TIME + $RESPONSE_TIME" | bc)
    else
        ((UNHEALTHY_COUNT++))
        echo "[$TIMESTAMP] ALERT: Health check failed - curl error" | tee -a "$ALERT_FILE"
        echo "[$TIMESTAMP] #$i/$TOTAL_CHECKS | Status: FAILED" | tee -a "$LOG_FILE"
    fi

    # Progress indicator
    if [ $((i % 60)) -eq 0 ]; then
        PROGRESS=$((i * 100 / TOTAL_CHECKS))
        echo "Progress: ${PROGRESS}% ($i/$TOTAL_CHECKS checks completed)" | tee -a "$LOG_FILE"
    fi

    sleep "$INTERVAL"
done

echo "" | tee -a "$LOG_FILE"
echo "============================================" | tee -a "$LOG_FILE"
echo "  24h Monitoring Complete" | tee -a "$LOG_FILE"
echo "============================================" | tee -a "$LOG_FILE"
echo "End time: $(date)" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

# Statistics
echo "=== Summary ===" | tee -a "$LOG_FILE"
echo "Total checks: $TOTAL_CHECKS" | tee -a "$LOG_FILE"
echo "Healthy: $HEALTHY_COUNT ($((HEALTHY_COUNT * 100 / TOTAL_CHECKS))%)" | tee -a "$LOG_FILE"
echo "Unhealthy: $UNHEALTHY_COUNT ($((UNHEALTHY_COUNT * 100 / TOTAL_CHECKS))%)" | tee -a "$LOG_FILE"

if [ "$HEALTHY_COUNT" -gt 0 ]; then
    AVG_RESPONSE=$(echo "scale=3; $TOTAL_RESPONSE_TIME / $HEALTHY_COUNT" | bc)
    echo "Average response time: ${AVG_RESPONSE}s" | tee -a "$LOG_FILE"
fi

echo "" | tee -a "$LOG_FILE"

# Memory growth analysis
echo "=== Memory Growth Analysis ===" | tee -a "$LOG_FILE"
FIRST_MEM=$(grep -m 1 "Memory:" "$LOG_FILE" | grep -oP 'Memory: \K[^ ]+' || echo "N/A")
LAST_MEM=$(grep "Memory:" "$LOG_FILE" | tail -1 | grep -oP 'Memory: \K[^ ]+' || echo "N/A")
echo "Initial memory: $FIRST_MEM" | tee -a "$LOG_FILE"
echo "Final memory: $LAST_MEM" | tee -a "$LOG_FILE"

echo "" | tee -a "$LOG_FILE"
echo "Full log: $LOG_FILE"
if [ -f "$ALERT_FILE" ]; then
    echo "Alerts: $ALERT_FILE"
fi

# Exit code: 0 if uptime >= 95%, 1 otherwise
UPTIME_PERCENT=$((HEALTHY_COUNT * 100 / TOTAL_CHECKS))
if [ "$UPTIME_PERCENT" -ge 95 ]; then
    echo "PASS: Uptime ${UPTIME_PERCENT}% >= 95%"
    exit 0
else
    echo "FAIL: Uptime ${UPTIME_PERCENT}% < 95%"
    exit 1
fi
