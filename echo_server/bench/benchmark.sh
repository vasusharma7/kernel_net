#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# benchmark.sh — Build the echo server and benchmark across worker counts.
#                Auto-detects available server binaries (epoll, io_uring).
#
# Usage:  ./benchmark.sh [--port PORT] [--server-workers N,N,N]
#                        [--connections N] [--requests N] [--size BYTES]
#                        [--perf]     # show syscall counts via perf stat
# ---------------------------------------------------------------------------
set -euo pipefail

PORT=8080
SERVER_WORKERS="1,2,4"
CONNECTIONS=10
REQUESTS=1000
SIZE=4096
PERF=false
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CLIENT="$PROJECT_DIR/bench_client"

usage() {
    echo "Usage: $0 [--port PORT] [--server-workers N,N,N] [--connections N]"
    echo "          [--requests N] [--size BYTES] [--perf]"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)           PORT="$2";           shift 2 ;;
        --server-workers) SERVER_WORKERS="$2"; shift 2 ;;
        --connections)    CONNECTIONS="$2";     shift 2 ;;
        --requests)       REQUESTS="$2";       shift 2 ;;
        --size)           SIZE="$2";           shift 2 ;;
        --perf)           PERF=true;           shift 1 ;;
        --help|-h)        usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# Build
echo "=== Building ==="
make -C "$PROJECT_DIR" clean 2>/dev/null || true
make -C "$PROJECT_DIR" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

IFS=',' read -ra WORKER_LIST <<< "$SERVER_WORKERS"

# Auto-detect available server binaries
SERVERS=()
for bin in echo_server echo_server_epoll echo_server_iouring echo_server_iouring_zc; do
    if [[ -x "$PROJECT_DIR/$bin" ]]; then
        SERVERS+=("$PROJECT_DIR/$bin")
    fi
done

if [[ ${#SERVERS[@]} -eq 0 ]]; then
    echo "ERROR: No server binary found."
    exit 1
fi

RESULTS="$PROJECT_DIR/results.txt"
echo "# Echo Server Benchmark Results" > "$RESULTS"
echo "# Date: $(date)" >> "$RESULTS"
echo "# Connections=${CONNECTIONS} Requests/conn=${REQUESTS} Payload=${SIZE}B" >> "$RESULTS"
echo "" >> "$RESULTS"

for SERVER in "${SERVERS[@]}"; do
    BINNAME=$(basename "$SERVER")
    echo ""
    echo "============================================================"
    echo "  Flavor: ${BINNAME}"
    echo "============================================================"

    for WORKERS in "${WORKER_LIST[@]}"; do
        echo ""
        echo "--- ${BINNAME} | Workers: ${WORKERS} ---"
        echo "" | tee -a "$RESULTS"

        # Start server
        "$SERVER" --port "$PORT" --workers "$WORKERS" &
        SERVER_PID=$!
        sleep 1

        # Capture syscall counts via perf stat (Linux only)
        PERF_FILE=""
        if $PERF && command -v perf &>/dev/null; then
            PERF_FILE="/tmp/perf_${BINNAME}_w${WORKERS}.txt"
            # perf stat -p runs until killed; prints summary on SIGINT
            # Note: may need sudo or perf_event_paranoid= -1 for syscall events
            perf stat -e syscalls:sys_enter_read,syscalls:sys_enter_write,syscalls:sys_enter_epoll_wait,syscalls:sys_enter_io_uring_enter,context-switches \
                -p "$SERVER_PID" -o "$PERF_FILE" 2>/dev/null &
            PERF_PID=$!
        fi

        # Run benchmark
        "$CLIENT" --port "$PORT" --connections "$CONNECTIONS" \
                  --requests "$REQUESTS" --size "$SIZE" --threads 2 \
            | tee -a "$RESULTS"

        # Stop perf and print syscall counts
        if [[ -n "$PERF_PID" ]]; then
            kill -INT "$PERF_PID" 2>/dev/null || true
            wait "$PERF_PID" 2>/dev/null || true
            echo "" | tee -a "$RESULTS"
            echo "  --- Server syscall counts ---" | tee -a "$RESULTS"
            if [[ -f "$PERF_FILE" ]]; then
                head -20 "$PERF_FILE" | grep -v "^$" | tee -a "$RESULTS"
                rm -f "$PERF_FILE"
            else
                echo "  (perf output not captured — try: sudo sysctl kernel.perf_event_paranoid=-1)" | tee -a "$RESULTS"
            fi
            echo "" | tee -a "$RESULTS"
        fi

        # Stop server
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        sleep 1
    done
done

echo ""
echo "=== Done ==="
echo "Results saved to: $RESULTS"