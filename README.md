# kernel_net

> **Networking projects at the intersection of Kernel and Userspace.**

This repository explores how different I/O multiplexing strategies affect network server performance. We built a TCP echo server in **four flavors** across **macOS and Linux**, benchmarked them head-to-head, and documented every bug, insight, and "aha!" moment along the way.

📖 **Read the full story**: [`TCP_ECHO_SERVER_BLOG.md`](./TCP_ECHO_SERVER_BLOG.md)

---

## The Flavors

| Flavor | Platform | I/O Mechanism | Threading | Mutex | Data Copies |
|--------|----------|---------------|-----------|-------|-------------|
| `kqueue` | macOS / BSD | `kqueue` + `kevent` | 1 net + N workers | `std::mutex` on task queue | 2 (kernel↔user↔kernel) |
| `epoll` | Linux | `epoll_wait` | 1 net + N workers | `std::mutex` on task queue | 2 (kernel↔user↔kernel) |
| `io_uring` | Linux 5.1+ | Async SQ/CQ ring buffers | 1 (single thread) | None | 2 (kernel↔user↔kernel) |
| `io_uring_zc` | Linux 5.6+ | Async + `SEND_ZC` | 1 (single thread) | None | 1 (zero-copy on send) |

All flavors implement the same protocol: accept a TCP connection, read data, echo it back, repeat until disconnect.

---

## Architecture

### epoll / kqueue (thread pool baseline)

```
[Network thread] ──read──→ [mutex lock] ──push──→ [std::queue] ──pop──→ [Worker thread] ──write──→
                              ↑                        ↓
                         pthread_mutex_lock      pthread_mutex_lock
```

The network thread runs an event loop (`epoll_wait` / `kevent`), reads data, and pushes tasks onto a shared `std::queue` protected by a `std::mutex`. Worker threads pop tasks and write the echo response. This is the standard production pattern for multi-threaded network servers.

### io_uring (single-threaded async)

```
[Single thread] ──submit recv──→ [kernel copies data] ──CQE──→ [process] ──submit send──→ [kernel copies data] ──CQE──→ ...
```

The kernel's TCP stack processes packets across all CPU cores in parallel. The single thread submits I/O requests via the Submission Queue (SQ — shared memory, no syscall) and collects results from the Completion Queue (CQ). Only `io_uring_submit()` and `io_uring_wait_cqe()` are actual syscalls — and they're batched.

---

## Benchmark Results (4096-byte payload, Linux VM)

```
                  epoll                io_uring
Workers   RPS       p99(ms)    RPS       p99(ms)
─────────────────────────────────────────────────
   1     43,421    0.080      55,661    0.061
   2     41,959    0.080      58,101    0.058
   4     39,283    0.088      58,427    0.054
```

**io_uring is ~35% faster than epoll at best.** Key observations:

- **epoll RPS drops with more workers** — mutex contention on the shared task queue becomes the bottleneck. More workers = more fighting over the lock.
- **io_uring RPS is flat** — it ignores `--workers` entirely. No thread pool, no mutex, no contention.
- **io_uring p99 is lower and stable** — no queuing delay from the shared task queue, no scheduling jitter from worker preemption.

### Where the 35% comes from

| Factor | Contribution | Why |
|--------|-------------|-----|
| Fewer syscalls | ~50% | epoll: 3 per echo (epoll_wait + read + write). io_uring: 1 (wait_cqe) |
| No mutex contention | ~30% | epoll pays ~2-4µs per echo even with 1 worker (mutex, cache miss, thread wakeup) |
| No context switches | ~20% | epoll workers get scheduled/descheduled; io_uring's single thread stays on one core |

### Why io_uring ZC didn't run

The `SEND_ZC` flavor requires Linux kernel ≥ 5.6 and liburing ≥ 2.2. The test VM's kernel was too old. For small payloads (64 bytes), `SEND_ZC` is actually **slower** than normal send because page-pinning (~1-2µs) dwarfs the memcpy cost (~50ns). The crossover point is around 64-256 KB.

---

## Project Layout

```
echo_server/
├── Makefile                          # Builds all flavors
├── common/
│   ├── thread_pool.h/.cpp           # std::queue + mutex + cv thread pool
│   └── server_interface.h           # Virtual interface all flavors implement
├── flavors/
│   ├── kqueue/                      # [macOS] kqueue + thread pool
│   ├── epoll/                       # [Linux] epoll + thread pool
│   ├── iouring/                     # [Linux] io_uring, single-threaded
│   └── iouring_zc/                  # [Linux] io_uring + SEND_ZC
└── bench/
    ├── bench_client.cpp             # Multi-threaded benchmark client
    └── benchmark.sh                 # Auto-detects flavors, runs benchmarks
```

---

## Quick Start

### Linux

```bash
# Install dependencies
sudo apt install liburing-dev     # needed for io_uring flavors
sudo apt install strace           # optional, for --perf syscall counting

# Build everything
cd echo_server
make -j$(nproc)

# Run all benchmarks
./bench/benchmark.sh

# Run with syscall counting
./bench/benchmark.sh --perf
```

### macOS

```bash
cd echo_server
make -j$(sysctl -n hw.ncpu)

# Run benchmarks
./bench/benchmark.sh
```

### Run individual tests

```bash
# Start a specific server
./echo_server_epoll --port 8080 --workers 2

# In another terminal, run the benchmark client
./bench_client --port 8080 --connections 10 --requests 1000 --size 4096 --threads 2
```

---

## Bugs We Hit (So You Don't Have To)

### 1. SQE not flushed on timeout
The initial `submit_accept()` prepared an SQE, but `io_uring_submit()` was only called after CQ iteration. On the first timeout, `continue` skipped phase 2 entirely — the accept SQE was never flushed. **Fix:** always reach phase 2 even on timeout.

### 2. Edge-triggered partial reads
With `EPOLLET`, if `read()` returns 2048 bytes out of 4096, the remaining data sits in the socket buffer but no new event fires. The client blocks forever. **Fix:** use level-triggered (`EPOLLIN` without `EPOLLET`).

### 3. Network thread closing fd while worker writes
If the network thread reads EOF (0 bytes) and closes the fd, the worker (still writing the echo) gets `EBADF`. **Fix:** the network thread removes the fd from epoll/kqueue and enqueues a close task — the worker owns the fd lifecycle.

### 4. TCP send buffer limit
At 8192-byte payloads with 4096-byte `BUF_SIZE`, the server split each echo into two writes. The second write hit the socket send buffer limit (~16KB default) and busy-spinned on `EAGAIN`. **Fix:** increase `BUF_SIZE` to 16384 and set `SO_SNDBUF` to 256KB.

---

## The Bigger Picture

```
                  epoll + threads         io_uring              io_uring + threads
                     │                        │                        │
Small payloads        ◄────── FASTEST ────────►                        │
(64B - 4KB)
                     │                        │                        │
Medium payloads      │              epoll ────►  io_uring ────►   (best)
(64KB - 1MB)         │              slower      faster          if CPU work
                     │                        │                        │
Large payloads       │      SEND_ZC / splice  ◄───  FASTEST ──────────►
(1MB+)               │      (zero-copy)       │      (if CPU work)
```

- **Small payloads (echo, Redis, memcached):** io_uring wins due to syscall elimination.
- **CPU-heavy work (JSON APIs, video transcoding):** io_uring + thread pool. One thread handles I/O, workers handle CPU.
- **Large file transfers:** SEND_ZC or `splice()` for true zero-copy.
- **macOS / BSD:** kqueue is your only option — same architecture as epoll.

---

## Further Reading

- [`TCP_ECHO_SERVER_BLOG.md`](./TCP_ECHO_SERVER_BLOG.md) — Full story with diagrams, code walkthroughs, and lessons learned
- [io_uring kernel docs](https://docs.kernel.org/networking/iou-zcrx.html) — Zero-copy receive documentation
- [liburing](https://github.com/axboe/liburing) — The io_uring library used in this project

## License

MIT
