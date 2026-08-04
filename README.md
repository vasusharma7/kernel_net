# kernel_net
Networking projects at the intersection of Kernel

## Projects

### `echo_server/` — Multi-threaded TCP Echo Server

A multi-threaded TCP echo server using **kqueue** for the network thread and
a `ThreadPool` consuming tasks from a `std::queue` protected by a `std::mutex`
+ `std::condition_variable`.

**Project layout**

```
echo_server/
├── Makefile
├── common/
│   ├── thread_pool.h/.cpp       # std::queue + mutex + cv thread pool
│   └── server_interface.h       # Virtual interface
├── flavors/kqueue/              # kqueue + kevent implementation
│   ├── main.cpp
│   ├── server.h
│   └── server.cpp
└── bench/
    ├── bench_client.cpp         # Benchmark client
    └── benchmark.sh             # Runs benchmarks across worker counts
```

**Architecture**

```
[kqueue network thread] ──enqueue──→ [std::queue + mutex] ──→ [worker threads]
       |                                                         |
   reads data                                              writes echo back
```

**Build**

```bash
cd echo_server
make -j$(sysctl -n hw.ncpu)
```

Produces `echo_server` (the server) and `bench_client`.

**Run**

```bash
# Start server with 2 workers
./echo_server --port 8080 --workers 2

# In another terminal, benchmark it
./bench_client --port 8080 --connections 10 --requests 1000 --size 64
```

**Benchmark**

```bash
bash bench/benchmark.sh
```

Iterates over worker counts (default: 1, 2, 4) and logs results to `results.txt`.

| Metric               | Description                |
|----------------------|----------------------------|
| Requests/sec (RPS)   | Throughput                 |
| p99 latency (ms)     | Tail latency               |
| Avg latency (ms)     | Mean latency               |

| Metric                    | Source                          |
|---------------------------|---------------------------------|
| **Requests/sec (RPS)**    | Benchmark client                |
| **p99 latency (ms)**      | Benchmark client                |
| **Avg latency (ms)**      | Benchmark client                |
| **Voluntary CS/sec**      | `/proc/self/status` (Linux)     |
| **Nonvoluntary CS/sec**   | `/proc/self/status` (Linux)     |
