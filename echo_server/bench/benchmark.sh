#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# benchmark.sh — Build the echo server and benchmark across worker counts.
#
# Usage:  ./benchmark.sh [--port PORT] [--server-workers N,N,N]
#                        [--connections N] [--requests N] [--size BYTES]
# ---------------------------------------------------------------------------
set -euo pipefail

PORT=8080
SERVER_WORKERS="1,2,4"
CONNECTIONS=10
REQUESTS=1000
SIZE=64
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SERVER="$PROJECT_DIR/echo_server"
CLIENT="$PROJECT_DIR/bench_client"

usage() {
    echo "Usage: $0 [--port PORT] [--server-workers N,N,N] [--connections N]"
    echo "          [--requests N] [--size BYTES]"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)           PORT="$2";           shift 2 ;;
        --server-workers) SERVER_WORKERS="$2"; shift 2 ;;
        --connections)    CONNECTIONS="$2";     shift 2 ;;
        --requests)       REQUESTS="$2";       shift 2 ;;
        --size)           SIZE="$2";           shift 2 ;;
        --help|-h)        usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# Build
echo "=== Building ==="
make -C "$PROJECT_DIR" clean 2>/dev/null || true
make -C "$PROJECT_DIR" -j$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

IFS=',' read -ra WORKER_LIST <<< "$SERVER_WORKERS"

echo ""
echo "============================================================"
echo "  Echo Server Benchmark"
echo "  ${CONNECTIONS} clients x ${REQUESTS} req/conn, ${SIZE}B payload"
echo "============================================================"

RESULTS="$PROJECT_DIR/results.txt"
echo "# Echo Server Benchmark Results" > "$RESULTS"
echo "# Date: $(date)" >> "$RESULTS"
echo "# Connections=${CONNECTIONS} Requests/conn=${REQUESTS} Payload=${SIZE}B" >> "$RESULTS"
echo "" >> "$RESULTS"

for WORKERS in "${WORKER_LIST[@]}"; do
    echo ""
    echo "========== Workers: ${WORKERS} =========="
    echo "" | tee -a "$RESULTS"

    "$SERVER" --port "$PORT" --workers "$WORKERS" &
    SERVER_PID=$!
    sleep 1

    "$CLIENT" --port "$PORT" --connections "$CONNECTIONS" \
              --requests "$REQUESTS" --size "$SIZE" --threads 2 \
        | tee -a "$RESULTS"

    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    sleep 1
done

echo ""
echo "=== Done ==="
echo "Results saved to: $RESULTS"