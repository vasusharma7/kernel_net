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

        # Capture syscall counts via strace -c (Linux only)
        STRACE_PID=""
        STRACE_FILE=""
        if $PERF && command -v strace &>/dev/null; then
            STRACE_FILE="/tmp/strace_${BINNAME}_w${WORKERS}.txt"
            # strace -c counts syscalls until killed, prints summary on exit
            strace -c -p "$SERVER_PID" -o "$STRACE_FILE" 2>/dev/null &
            STRACE_PID=$!
        fi

        # Run benchmark
        "$CLIENT" --port "$PORT" --connections "$CONNECTIONS" \
                  --requests "$REQUESTS" --size "$SIZE" --threads 2 \
            | tee -a "$RESULTS"

        # Stop strace and print syscall counts
        if [[ -n "$STRACE_PID" ]]; then
            kill -INT "$STRACE_PID" 2>/dev/null || true
            wait "$STRACE_PID" 2>/dev/null || true
            echo "" | tee -a "$RESULTS"
            echo "  --- Server syscall counts ---" | tee -a "$RESULTS"
            if [[ -f "$STRACE_FILE" ]]; then
                grep -E "read|write|epoll|io_uring|futex|wait|total|%" "$STRACE_FILE" | head -15 | tee -a "$RESULTS"
                rm -f "$STRACE_FILE"
            else
                echo "  (strace output not captured)" | tee -a "$RESULTS"
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