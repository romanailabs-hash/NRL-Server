#!/usr/bin/env bash
# bench_web.sh — Load & latency benchmark for NRL-Server
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
TOKEN="${NRL_SERVER_TOKEN:-}"
AUTH_HEADER=""
[ -n "$TOKEN" ] && AUTH_HEADER="-H Authorization: Bearer $TOKEN"

echo "NRL-Server Benchmark Script"
echo "=========================="
echo "Target: $BASE_URL"
echo ""

# Test 1: Health endpoint latency
if command -v curl &>/dev/null; then
    echo "[Test 1] /health latency (100 requests)..."
    total_time=0
    for i in $(seq 1 100); do
        time_start=$(date +%s%N)
        curl -sf -o /dev/null "$BASE_URL/health" 2>/dev/null || true
        time_end=$(date +%s%N)
        total_time=$((total_time + time_end - time_start))
    done
    avg_ms=$((total_time / 100 / 1000000))
    echo "  Average latency: ${avg_ms}ms"
    echo ""
    
    # Test 2: NRL tick
    echo "[Test 2] /v1/tick (10 iterations)..."
    for i in $(seq 1 10); do
        curl -sf $AUTH_HEADER -H 'Content-Type: application/json'             -d '{"steps":10000,"input_seed":42}'             "$BASE_URL/v1/tick" 2>/dev/null || echo "  Request $i failed"
    done
    echo ""
    
    # Test 3: Run a sandbox command
    echo "[Test 3] /v1/run (execute echo hello)..."
    curl -sf $AUTH_HEADER -H 'Content-Type: application/json'         -d '{"rootfs":"","cmd":["/bin/echo","hello"],"timeout_ms":5000,"mem_mb":64,"cpu_quota_pct":50,"pids_max":16}'         "$BASE_URL/v1/run" 2>/dev/null || echo "  Run request failed"
    echo ""
else
    echo "curl is required for benchmarks."
    exit 1
fi

echo "Benchmark complete."
