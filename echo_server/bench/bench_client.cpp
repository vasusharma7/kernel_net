// -----------------------------------------------------------------------
// TCP echo benchmark client
//
// Opens concurrent connections, sends a fixed-size message, waits for
// the echo, and reports: RPS, p99 latency, avg latency.
// -----------------------------------------------------------------------

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// -----------------------------------------------------------------------
// Benchmark result
// -----------------------------------------------------------------------
struct Stats {
    double rps;
    double p99_latency_ms;
    double avg_latency_ms;
};

// -----------------------------------------------------------------------
// Core benchmark
// -----------------------------------------------------------------------
static Stats run_benchmark(const std::string& host, uint16_t port,
                           size_t num_connections, size_t requests_per_conn,
                           size_t message_size, size_t num_threads) {

    std::vector<double> latencies_ms;
    std::mutex          lat_mutex;
    std::atomic<size_t> completed{0};
    std::atomic<bool>   start_flag{false};

    std::string message(message_size, 'a');

    // Each thread opens a subset of connections
    auto thread_func = [&](size_t conns) {
        while (!start_flag)
            std::this_thread::yield();

        for (size_t c = 0; c < conns; ++c) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                std::cerr << "socket: " << std::strerror(errno) << "\n";
                continue;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
                std::cerr << "inet_pton failed for " << host << "\n";
                close(fd);
                continue;
            }

            if (connect(fd, reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr)) < 0) {
                std::cerr << "connect: " << std::strerror(errno) << "\n";
                close(fd);
                continue;
            }

            for (size_t r = 0; r < requests_per_conn; ++r) {
                auto t1 = std::chrono::steady_clock::now();

                // Send
                size_t sent = 0;
                while (sent < message.size()) {
                    ssize_t n = write(fd, message.data() + sent,
                                      message.size() - sent);
                    if (n < 0) break;
                    sent += static_cast<size_t>(n);
                }

                // Read echo (exactly message_size bytes)
                std::string reply(message_size, '\0');
                size_t recvd = 0;
                while (recvd < message.size()) {
                    ssize_t n = read(fd, (void *)reply.data() + recvd,
                                     message.size() - recvd);
                    if (n <= 0) break;
                    recvd += static_cast<size_t>(n);
                }

                auto t2 = std::chrono::steady_clock::now();

                if (recvd == message.size()) {
                    double ms = std::chrono::duration<double, std::milli>(
                                    t2 - t1)
                                    .count();
                    std::lock_guard<std::mutex> lock(lat_mutex);
                    latencies_ms.push_back(ms);
                }

                completed.fetch_add(1, std::memory_order_relaxed);
            }

            close(fd);
        }
    };

    // Distribute connections across threads
    size_t conns_per = num_connections / num_threads;
    size_t extra     = num_connections % num_threads;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    auto start_time  = std::chrono::steady_clock::now();

    for (size_t t = 0; t < num_threads; ++t) {
        size_t cpt = conns_per + (t < extra ? 1 : 0);
        threads.emplace_back(thread_func, cpt);
    }

    start_flag = true;

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed_sec =
        std::chrono::duration<double>(end_time - start_time).count();

    if (latencies_ms.empty()) {
        return {0, 0, 0};
    }

    std::sort(latencies_ms.begin(), latencies_ms.end());
    double rps   = static_cast<double>(completed.load()) / elapsed_sec;
    size_t p99_idx =
        static_cast<size_t>(std::ceil(0.99 * latencies_ms.size())) - 1;
    double p99  = latencies_ms[p99_idx];
    double avg  = std::accumulate(latencies_ms.begin(), latencies_ms.end(),
                                  0.0) /
                  static_cast<double>(latencies_ms.size());

    return {rps, p99, avg};
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::string host         = "127.0.0.1";
    uint16_t    port         = 8080;
    size_t      connections  = 10;
    size_t      requests     = 1000;   // per connection
    size_t      message_size = 64;
    size_t      threads      = 2;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (std::strcmp(argv[i], "--connections") == 0 && i + 1 < argc)
            connections = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--requests") == 0 && i + 1 < argc)
            requests = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            message_size = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            threads = std::stoul(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: bench_client [options]\n"
                      << "  --host HOST       (default 127.0.0.1)\n"
                      << "  --port PORT       (default 8080)\n"
                      << "  --connections N   (default 10)\n"
                      << "  --requests N      (default 1000)\n"
                      << "  --size BYTES      (default 64)\n"
                      << "  --threads N       (default 2)\n";
            return 0;
        }
    }

    size_t total_req = connections * requests;

    std::cout << "========================================\n";
    std::cout << "  Echo Server Benchmark\n";
    std::cout << "========================================\n";
    std::cout << "  connections:  " << connections << "\n";
    std::cout << "  requests:     " << requests << " per connection\n";
    std::cout << "  total req:    " << total_req << "\n";
    std::cout << "  message size: " << message_size << " bytes\n";
    std::cout << "  client threads:" << threads << "\n";
    std::cout << "  target:       " << host << ":" << port << "\n\n";

    Stats s = run_benchmark(host, port, connections, requests,
                            message_size, threads);

    std::cout << "------------  Results  -------------\n";
    std::cout << "  Requests/sec:         " << s.rps << "\n";
    std::cout << "  p99 latency (ms):     " << s.p99_latency_ms << "\n";
    std::cout << "  Avg latency (ms):     " << s.avg_latency_ms << "\n";
    std::cout << "========================================\n";

    return 0;
}